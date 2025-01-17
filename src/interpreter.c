// This file is a part of Julia. License is MIT: https://julialang.org/license

#include <stdlib.h>
#include <setjmp.h>
#ifdef _OS_WINDOWS_
#include <malloc.h>
#endif
#include "julia.h"
#include "julia_internal.h"
#include "builtin_proto.h"
#include "julia_assert.h"
#include "dyncall.h"
#include "interpreter.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    jl_code_info_t *src; // contains the names and number of slots
    jl_method_instance_t *mi; // MethodInstance we're executing, or NULL if toplevel
    jl_module_t *module; // context for globals
    jl_value_t **locals; // slots for holding local slots and ssavalues
    jl_svec_t *sparam_vals; // method static parameters, if eval-ing a method body
    size_t ip; // Leak the currently-evaluating statement index to backtrace capture
    int preevaluation; // use special rules for pre-evaluating expressions (deprecated--only for ccall handling)
    int continue_at; // statement index to jump to after leaving exception handler (0 if none)
    int jit;
} interpreter_state;


// general alloca rules are incompatible on C and C++, so define a macro that deals with the difference
#ifdef __cplusplus
#define JL_CPPALLOCA(var,n)                                                         \
  var = (decltype(var))alloca((n))
#else
#define JL_CPPALLOCA(var,n)                                                         \
  JL_GCC_IGNORE_START("-Wc++-compat")                                               \
  var = alloca((n));                                                                \
  JL_GCC_IGNORE_STOP
#endif

#ifdef __clang_gcanalyzer__

extern void JL_GC_ENABLEFRAME(interpreter_state*) JL_NOTSAFEPOINT;

// This is necessary, because otherwise the analyzer considers this undefined
// behavior and terminates the exploration
#define JL_GC_PUSHFRAME(frame,locals,n)     \
  JL_CPPALLOCA(frame, sizeof(*frame)+((n) * sizeof(jl_value_t*)));  \
  memset(&frame[1], 0, sizeof(void*) * n); \
  _JL_GC_PUSHARGS((jl_value_t**)&frame[1], n); \
  locals = (jl_value_t**)&frame[1];

#else

#define JL_GC_ENCODE_PUSHFRAME(n)  ((((size_t)(n))<<2)|2)

#define JL_GC_PUSHFRAME(frame,locals,n)                                             \
  JL_CPPALLOCA(frame, sizeof(*frame)+(((n)+3)*sizeof(jl_value_t*)));                \
  ((void**)&frame[1])[0] = NULL;                                                    \
  ((void**)&frame[1])[1] = (void*)JL_GC_ENCODE_PUSHFRAME(n);                        \
  ((void**)&frame[1])[2] = jl_pgcstack;                                             \
  memset(&((void**)&frame[1])[3], 0, (n)*sizeof(jl_value_t*));                      \
  jl_pgcstack = (jl_gcframe_t*)&(((void**)&frame[1])[1]);                           \
  locals = &((jl_value_t**)&frame[1])[3];

// we define this separately so that we can populate the frame before we add it to the backtrace
// it's recommended to mark the containing function with NOINLINE, though not essential
#define JL_GC_ENABLEFRAME(frame) \
  ((void**)&frame[1])[0] = __builtin_frame_address(0);

#endif


static jl_value_t *eval_value(jl_value_t *e, interpreter_state *s);
static jl_value_t *eval_body(jl_array_t *stmts, interpreter_state *s, size_t ip, int toplevel);

// method definition form

static jl_value_t *eval_methoddef(jl_expr_t *ex, interpreter_state *s)
{
    jl_value_t **args = jl_array_ptr_data(ex->args);

    // generic function definition
    if (jl_expr_nargs(ex) == 1) {
        jl_value_t **args = jl_array_ptr_data(ex->args);
        jl_sym_t *fname = (jl_sym_t*)args[0];
        jl_module_t *modu = s->module;
        if (jl_is_globalref(fname)) {
            modu = jl_globalref_mod(fname);
            fname = jl_globalref_name(fname);
        }
        if (!jl_is_symbol(fname)) {
            jl_error("method: invalid declaration");
        }
        jl_value_t *bp_owner = (jl_value_t*)modu;
        jl_binding_t *b = jl_get_binding_for_method_def(modu, fname);
        _Atomic(jl_value_t*) *bp = &b->value;
        jl_value_t *gf = jl_generic_function_def(b->name, b->owner, bp, bp_owner, b);
        return gf;
    }

    jl_value_t *atypes = NULL, *meth = NULL, *fname = NULL;
    JL_GC_PUSH3(&atypes, &meth, &fname);

    fname = eval_value(args[0], s);
    jl_methtable_t *mt = NULL;
    if (jl_typeis(fname, jl_methtable_type)) {
        mt = (jl_methtable_t*)fname;
    }
    atypes = eval_value(args[1], s);
    meth = eval_value(args[2], s);
    jl_method_def((jl_svec_t*)atypes, mt, (jl_code_info_t*)meth, s->module);
    JL_GC_POP();
    return jl_nothing;
}

// expression evaluator
JL_DLLEXPORT void* (*jl_staticjit_get_cache)(jl_method_instance_t*, size_t) = NULL;
JL_DLLEXPORT void jl_staticjit_set_cache_geter(void* ptr){
    jl_staticjit_get_cache = (void* (*)(jl_method_instance_t*, size_t))ptr;
}

JL_DLLEXPORT void* (*jl_get_cfunction_ptr)(jl_method_instance_t*, size_t) = NULL;
JL_DLLEXPORT void jl_set_get_cfunction_ptr(void* ptr){
    jl_get_cfunction_ptr = (void* (*)(jl_method_instance_t*, size_t))ptr;
}

static jl_value_t *do_call(jl_value_t **args, size_t nargs, interpreter_state *s)
{
    jl_value_t **argv;
    assert(nargs >= 1);
    JL_GC_PUSHARGS(argv, nargs);
    size_t i;
    for (i = 0; i < nargs; i++)
        argv[i] = eval_value(args[i], s);
    jl_value_t *result = NULL;
    result = jl_apply(argv, nargs);
    JL_GC_POP();
    return result;
}

static jl_value_t *do_invoke(jl_value_t **args, size_t nargs, interpreter_state *s)
{
    jl_value_t **argv;
    assert(nargs >= 2);
    JL_GC_PUSHARGS(argv, nargs - 1);
    size_t i;
    for (i = 1; i < nargs; i++)
        argv[i] = eval_value(args[i], s);
    jl_method_instance_t *meth = (jl_method_instance_t*)args[0];
    assert(jl_is_method_instance(meth));
    jl_value_t *result = jl_invoke(argv[1], &argv[2], nargs - 2, meth);
    JL_GC_POP();
    return result;
}

jl_value_t *jl_eval_global_var(jl_module_t *m, jl_sym_t *e)
{
    jl_value_t *v = jl_get_global(m, e);
    if (v == NULL)
        jl_undefined_var_error(e);
    return v;
}

static int jl_source_nslots(jl_code_info_t *src) JL_NOTSAFEPOINT
{
    return jl_array_len(src->slotflags);
}

static int jl_source_nssavalues(jl_code_info_t *src) JL_NOTSAFEPOINT
{
    return jl_is_long(src->ssavaluetypes) ? jl_unbox_long(src->ssavaluetypes) : jl_array_len(src->ssavaluetypes);
}

static void eval_stmt_value(jl_value_t *stmt, interpreter_state *s)
{
    jl_value_t *res = eval_value(stmt, s);
    s->locals[jl_source_nslots(s->src) + s->ip] = res;
}
static jl_value_t* jl_new_ptr(jl_value_t* ptype, jl_value_t* pvalue){
    return jl_new_bits(ptype, (void*)pvalue);
}
// Input parameter must be set correctly, otherwise calling conversion will be broken
static void setInputParameter(DCCallVM* vm, jl_value_t* _dt, jl_value_t* e){
    // Firstly we set floating point type
    if (_dt == (jl_value_t*)jl_float16_type){
        assert("Float16 is not supported!");
    }
    else if(_dt == (jl_value_t*)jl_float32_type){
        dcArgFloat(vm, jl_unbox_float32(e));
        return;
    }
    else if(_dt == (jl_value_t*)jl_float64_type){
        dcArgDouble(vm, jl_unbox_float64(e));
        return;
    }
    // Then we set pointer type
    if (_dt==(jl_value_t*)jl_any_type){
        dcArgPointer(vm,e);    
    }
    else if (jl_is_abstract_ref_type(_dt)){
        dcArgPointer(vm,*(void**)e); 
    }
    else if (jl_is_cpointer_type(_dt)){
        dcArgPointer(vm,(void*)(*((uint64_t*)e)));
    }
    else if (jl_is_primitivetype(_dt)){
        size_t size = jl_datatype_size(_dt);
        switch (size)
        {
        case 0:
            break;
        case 1:
            dcArgChar(vm,jl_unbox_bool(e));
            break;
        case 2:
            dcArgShort(vm,jl_unbox_int16(e));
            break;
        case 4:
            dcArgInt(vm,jl_unbox_uint32(e));
            break;
        case 8:
            dcArgLong(vm,jl_unbox_int64(e));
            break;
        case 16:
            dcArgLongLong(vm,*(long long int*)(e));
            break;
        default:
            jl_safe_printf("Might be unsupported primitive input type, at file %s:%d \n",jl_filename,jl_lineno);
            jl_(_dt);
            exit(1);
            break;
        }
    }
    else{
        jl_safe_printf("Might be unsupported primitive input type, at file %s:%d \n",jl_filename,jl_lineno);
        jl_(_dt);
        dcArgPointer(vm,e);
    }
}

static jl_value_t *eval_value(jl_value_t *e, interpreter_state *s)
{
    jl_code_info_t *src = s->src;
    if (jl_is_ssavalue(e)) {
        ssize_t id = ((jl_ssavalue_t*)e)->id - 1;
        if (src == NULL || id >= jl_source_nssavalues(src) || id < 0 || s->locals == NULL)
            jl_error("access to invalid SSAValue");
        else
            return s->locals[jl_source_nslots(src) + id];
    }
    if (jl_is_slot(e) || jl_is_argument(e)) {
        ssize_t n = jl_slot_number(e);
        if (src == NULL || n > jl_source_nslots(src) || n < 1 || s->locals == NULL)
            jl_error("access to invalid slot number");
        jl_value_t *v = s->locals[n - 1];
        if (v == NULL)
            jl_undefined_var_error((jl_sym_t*)jl_array_ptr_ref(src->slotnames, n - 1));
        return v;
    }
    if (jl_is_quotenode(e)) {
        return jl_quotenode_value(e);
    }
    if (jl_is_globalref(e)) {
        return jl_eval_global_var(jl_globalref_mod(e), jl_globalref_name(e));
    }
    if (jl_is_symbol(e)) {  // bare symbols appear in toplevel exprs not wrapped in `thunk`
        return jl_eval_global_var(s->module, (jl_sym_t*)e);
    }
    if (jl_is_pinode(e)) {
        jl_value_t *val = eval_value(jl_fieldref_noalloc(e, 0), s);
#ifndef JL_NDEBUG
        JL_GC_PUSH1(&val);
        jl_typeassert(val, jl_fieldref_noalloc(e, 1));
        JL_GC_POP();
#endif
        return val;
    }
    assert(!jl_is_phinode(e) && !jl_is_phicnode(e) && !jl_is_upsilonnode(e) && "malformed IR");
    if (!jl_is_expr(e))
        return e;
    jl_expr_t *ex = (jl_expr_t*)e;
    jl_value_t **args = jl_array_ptr_data(ex->args);
    size_t nargs = jl_array_len(ex->args);
    jl_sym_t *head = ex->head;
    if (head == jl_call_sym) {
        return do_call(args, nargs, s);
    }
    else if (head == jl_invoke_sym) {
        return do_invoke(args, nargs, s);
    }
    else if (head == jl_invoke_modify_sym) {
        return do_call(args + 1, nargs - 1, s);
    }
    else if (head == jl_isdefined_sym) {
        jl_value_t *sym = args[0];
        int defined = 0;
        if (jl_is_slot(sym) || jl_is_argument(sym)) {
            ssize_t n = jl_slot_number(sym);
            if (src == NULL || n > jl_source_nslots(src) || n < 1 || s->locals == NULL)
                jl_error("access to invalid slot number");
            defined = s->locals[n - 1] != NULL;
        }
        else if (jl_is_globalref(sym)) {
            defined = jl_boundp(jl_globalref_mod(sym), jl_globalref_name(sym));
        }
        else if (jl_is_symbol(sym)) {
            defined = jl_boundp(s->module, (jl_sym_t*)sym);
        }
        else if (jl_is_expr(sym) && ((jl_expr_t*)sym)->head == jl_static_parameter_sym) {
            ssize_t n = jl_unbox_long(jl_exprarg(sym, 0));
            assert(n > 0);
            if (s->sparam_vals && n <= jl_svec_len(s->sparam_vals)) {
                jl_value_t *sp = jl_svecref(s->sparam_vals, n - 1);
                defined = !jl_is_typevar(sp);
            }
            else {
                // static parameter val unknown needs to be an error for ccall
                jl_error("could not determine static parameter value");
            }
        }
        else {
            assert(0 && "malformed isdefined expression");
        }
        return defined ? jl_true : jl_false;
    }
    else if (head == jl_throw_undef_if_not_sym) {
        jl_value_t *cond = eval_value(args[1], s);
        assert(jl_is_bool(cond));
        if (cond == jl_false) {
            jl_sym_t *var = (jl_sym_t*)args[0];
            if (var == jl_getfield_undefref_sym)
                jl_throw(jl_undefref_exception);
            else
                jl_undefined_var_error(var);
        }
        return jl_nothing;
    }
    else if (head == jl_new_sym) {
        jl_value_t **argv;
        JL_GC_PUSHARGS(argv, nargs);
        for (size_t i = 0; i < nargs; i++)
            argv[i] = eval_value(args[i], s);
        jl_value_t *v = jl_new_structv((jl_datatype_t*)argv[0], &argv[1], nargs - 1);
        JL_GC_POP();
        return v;
    }
    else if (head == jl_splatnew_sym) {
        jl_value_t **argv;
        JL_GC_PUSHARGS(argv, 2);
        argv[0] = eval_value(args[0], s);
        argv[1] = eval_value(args[1], s);
        jl_value_t *v = jl_new_structt((jl_datatype_t*)argv[0], argv[1]);
        JL_GC_POP();
        return v;
    }
    else if (head == jl_new_opaque_closure_sym) {
        jl_value_t **argv;
        JL_GC_PUSHARGS(argv, nargs);
        for (size_t i = 0; i < nargs; i++)
            argv[i] = eval_value(args[i], s);
        JL_NARGSV(new_opaque_closure, 5);
        jl_value_t *ret = (jl_value_t*)jl_new_opaque_closure((jl_tupletype_t*)argv[0], argv[1], argv[2],
            argv[3], argv[4], argv+5, nargs-5);
        JL_GC_POP();
        return ret;
    }
    else if (head == jl_static_parameter_sym) {
        ssize_t n = jl_unbox_long(args[0]);
        assert(n > 0);
        if (s->sparam_vals && n <= jl_svec_len(s->sparam_vals)) {
            jl_value_t *sp = jl_svecref(s->sparam_vals, n - 1);
            if (jl_is_typevar(sp) && !s->preevaluation)
                jl_undefined_var_error(((jl_tvar_t*)sp)->name);
            return sp;
        }
        // static parameter val unknown needs to be an error for ccall
        jl_error("could not determine static parameter value");
    }
    else if (head == jl_copyast_sym) {
        return jl_copy_ast(eval_value(args[0], s));
    }
    else if (head == jl_exc_sym) {
        return jl_current_exception();
    }
    else if (head == jl_boundscheck_sym) {
        return jl_true;
    }
    else if (head == jl_meta_sym || head == jl_coverageeffect_sym || head == jl_inbounds_sym || head == jl_loopinfo_sym ||
             head == jl_aliasscope_sym || head == jl_popaliasscope_sym || head == jl_inline_sym || head == jl_noinline_sym) {
        return jl_nothing;
    }
    else if (head == jl_gc_preserve_begin_sym || head == jl_gc_preserve_end_sym) {
        // The interpreter generally keeps values that were assigned in this scope
        // rooted. If the interpreter learns to be more aggressive here, we may
        // want to explicitly root these values.
        return jl_nothing;
    }
    else if (head == jl_method_sym && nargs == 1) {
        return eval_methoddef(ex, s);
    }
    else if (head == jl_foreigncall_sym) {
        //assert(jl_is_symbol(args[0]));
        //Expr(:foreigncall, pointer, rettype, (argtypes...), nreq, cconv, args..., roots...)
        //args is (pointer, rettype, (argtypes...), nreq, cconv, args..., roots...)
        // pointer + rettype + argtype + cconv + args
        jl_value_t **argv;
        JL_GC_PUSHARGS(argv, nargs);
        for (size_t i = 0; i < nargs; i++)
            argv[i] = eval_value(args[i], s);
        size_t vararg = jl_unbox_long(argv[3]); // if vararg
        if (vararg!=0){
            jl_error("vararg doesn't support");
        };
        jl_value_t* might_f = argv[0];
        jl_sym_t* fname = NULL;
        jl_sym_t* flib = NULL;
        void* fptr = NULL;
        if (jl_is_symbol(might_f)){
            fname = (jl_sym_t*)might_f;
        }
        else if (jl_is_string(might_f)){
            fname = jl_symbol_n(jl_string_ptr(might_f),jl_string_len(might_f));
        }
        else if (jl_is_cpointer_type(jl_typeof(might_f)) || jl_is_uint64(might_f) || jl_is_int64(might_f)){
            fptr = (void*)(*(uint64_t*)(might_f));
            if (fptr == NULL){
                // we try to call a null pointer, which is bad;
                JL_GC_POP();
                jl_error("Try to call a null pointer");
            }
        }
        else if (jl_is_tuple(might_f)){
            assert(jl_nfields(might_f) == 2);
            jl_value_t *t0 = jl_fieldref(might_f, 0);
            if (jl_is_symbol(t0)){
                fname = (jl_sym_t*)t0;
            }
            else if (jl_is_string(t0)){
                fname = jl_symbol(jl_string_ptr(t0));
            }
            else{
                exit(1);
                assert("Shouldn't be here");
            }
            jl_value_t *t1 = jl_fieldref(might_f, 1);
            if (jl_is_symbol(t1)){
                flib = (jl_sym_t*)t1;
            }
            else if (jl_is_string(t1)){
                flib = jl_symbol(jl_string_ptr(t1));
            }
            else{
                exit(1);
                assert("Shouldn't be here");
            }
        }
        else{
            jl_safe_printf("Invalid ccall arguments");
            exit(1);
        }
        // need to verify the return type
        jl_value_t *rt = argv[1];
        assert(jl_is_svec(argv[2]));
        jl_svec_t *at = (jl_svec_t*)argv[2];
        int ninput = jl_svec_len(at);
        jl_unionall_t *unionall = ((s->mi!=NULL)&&jl_is_method(s->mi->def.method) && jl_is_unionall(s->mi->def.method->sig))
        ? (jl_unionall_t*)s->mi->def.method->sig
        : NULL;
        if (unionall!=NULL){
            // we need to instance rt and at, since they might contain type variables
            argv[1] = jl_instantiate_type_in_env(rt,unionall,jl_svec_data(s->sparam_vals));
            rt = argv[1];
            jl_value_t **every_arg_type;
            JL_GC_PUSHARGS(every_arg_type, ninput);
            for (int i=0;i<ninput;i++){
                every_arg_type[i] = jl_instantiate_type_in_env(jl_svec_ref(at,i),unionall,jl_svec_data(s->sparam_vals));
            };
            argv[2] = (jl_value_t*)jl_alloc_svec_uninit(ninput);
            //TODO: is this safe?
            for (int i=0;i<ninput;i++){
                jl_svecset(argv[2],i,every_arg_type[i]);
            };
            at = (jl_svec_t*)argv[2];
            JL_GC_POP();
        };
        assert(jl_is_symbol(argv[4]));
        for (int k=0;k<ninput;k++){
            jl_value_t* inputtype = jl_svec_ref(at,k);
            jl_value_t* input_value = argv[5+k];
            if (!jl_types_equal(jl_typeof(input_value), inputtype)){
                if (jl_is_primitivetype(inputtype) && jl_is_primitivetype(jl_typeof(input_value))){
                    jl_value_t* cf = jl_atomic_load_relaxed(&(jl_get_binding(jl_core_module, jl_symbol("convert"))->value));
                    argv[5+k] = jl_apply_generic(cf,&input_value,1);
                }
                //jl_printf(JL_STDOUT, "Ccall might be error: with input type ");
                //jl_(inputtype);
                //jl_printf(JL_STDOUT, ", where is expected to be ");
                //jl_(jl_typeof(input_value));
            }
        }
        //jl_sym_t *cc_sym = (jl_sym_t*)argv[4];
        jl_value_t* r = NULL;
        if (fname==jl_symbol("jl_value_ptr")){
            assert(ninput==1);
            if (rt==(jl_value_t*)jl_any_type){
                assert(jl_is_cpointer_type(jl_svec_ref(at,0)));
                // ccall(:jl_value_ptr,Any,(Ptr{...},),jobj,...);
                r = (jl_value_t*)(jl_unbox_uint64(argv[5]));
            }
            else{
                // ccall(:jl_value_ptr,Ptr{...},(Any,),jobj,...);
                assert(jl_is_cpointer_type(rt));
                assert(jl_svec_ref(at,0)==(jl_value_t*)jl_any_type);
                jl_value_t **pointer_root;
                JL_GC_PUSHARGS(pointer_root, 1);
                pointer_root[0] = jl_box_uint64((uint64_t)argv[5]);
                r = jl_new_ptr(rt,pointer_root[0]);
                JL_GC_POP();
                //jl_error("Shouldn't be here!");
            };
            JL_GC_POP();
            return r;
        };
        // special call to construct symbol from a string
        if (fname==jl_symbol("jl_symbol_n")){
            // Input is type of (Ptr{UInt8},Int);
            // TODO: test this for 32bit platform
            const char* ptr = (const char *)jl_unbox_int64(argv[5]);
            size_t len = jl_unbox_int64(argv[6]);
            jl_value_t* new_sym = (jl_value_t*)jl_symbol_n(ptr,len);
            JL_GC_POP();
            return new_sym;
        }
        if (fname==jl_symbol("jl_symbol_name")){
            // Input is type of (Any,), that is a symbol, return a Ptr{UInt8};
            // TODO: test this for 32bit platform
            const char* ptr = jl_symbol_name((jl_sym_t*)(argv[5]));
            assert(jl_is_cpointer_type(rt));
            jl_value_t **pointer_root;
            JL_GC_PUSHARGS(pointer_root, 1);
            pointer_root[0] = jl_box_uint64((uint64_t)ptr);
            r = jl_new_ptr(rt,pointer_root[0]);
            JL_GC_POP();
            JL_GC_POP();
            return r;
        }
        if (fname==jl_symbol("jl_dlsym")){
            // Input is type of (Any,), that is a symbol, return a Ptr{UInt8};
            // TODO: test this for 32bit platform
            void** libpointer = *((void***)argv[5]);
            const char* cstring = *((const char**)argv[6]);
            void** store = *((void***)argv[7]);
            int throw_err = jl_unbox_int32(argv[8]);
            int raw_ptr = jl_dlsym(libpointer,cstring,store,throw_err);
            jl_value_t **pointer_root;
            JL_GC_PUSHARGS(pointer_root, 1);
            jl_value_t* ptr_int = jl_box_uint32(raw_ptr);
            pointer_root[0] = ptr_int;
            // we need to invoke the constructor for Ptr{...} to create a Ptr pointer
            // TODO: how to directly construct a Ptr{...}?
            r = jl_new_ptr(rt,ptr_int);
            //jl_safe_printf("After jl_string_ptr:%p",raw_ptr);
            //jl_(r);
            JL_GC_POP();
            JL_GC_POP();
            return r;
        }
        // If the input is not a raw pointer
        if (fptr == NULL){
            assert(jl_libjulia_internal_handle!=NULL);
            char *fname_cstr = jl_symbol_name(fname);
            int l = strlen(fname_cstr);
            char ifname[l+2];
            ifname[0] = 'i';
            ifname[l+1] = '\0';
            memcpy(&(ifname[1]),fname_cstr,l);
            //printf((const char*)ifname);
            //printf("\n");
            void* libhandle = jl_libjulia_internal_handle;
            if (flib != NULL){
                const char* s = jl_symbol_name(flib);
                libhandle = jl_get_library_(s,1);
            }
            jl_dlsym(libhandle,(const char*)ifname,&fptr,0);
            if (fptr == NULL){
                jl_dlsym(libhandle,(const char*)fname_cstr,&fptr,0);
            };
            if (fptr==NULL){
                //if (fname == jl_symbol("jl_compile_methodinst")){
                jl_dlsym(jl_RTLD_DEFAULT_handle,(const char*)fname_cstr,&fptr,1);
                /*
                }
                else{
                    jl_(fname);
                    jl_error("Symbol not found,here");
                };
                */
            };
        }
        DCCallVM* vm = dcNewCallVM(4096);
        dcMode(vm, DC_CALL_C_DEFAULT);
        dcReset(vm);
        for (int k=0;k<ninput;k++){
            jl_value_t* inputtype = jl_svec_ref(at,k);
            jl_value_t* input_value = argv[5+k];
            setInputParameter(vm, inputtype, input_value);
        }
        if (rt==(jl_value_t*)jl_any_type||jl_is_array_type(rt)){
            r = (jl_value_t*)dcCallPointer(vm, (DCpointer)fptr);
        }
        else if (rt==(jl_value_t*)jl_int64_type){
            r = jl_box_int64((int64_t)dcCallLong(vm, (DCpointer)fptr));
        }
        else if (rt==(jl_value_t*)jl_uint64_type){
            r = jl_box_uint64((uint64_t)dcCallLong(vm, (DCpointer)fptr));
        }
        else if (rt==(jl_value_t*)jl_int32_type){
            r = jl_box_int32((int32_t)dcCallInt(vm, (DCpointer)fptr));
        }
        else if (rt==(jl_value_t*)jl_uint32_type){
            r = jl_box_uint32((uint32_t)dcCallInt(vm, (DCpointer)fptr));
        }
        else if (rt==(jl_value_t*)jl_nothing_type){
            dcCallVoid(vm, (DCpointer)fptr);
            r = jl_nothing;
        }
        else if (rt==(jl_value_t*)jl_bool_type){
            r = jl_box_bool(dcCallChar(vm, (DCpointer)fptr));
        }
        else if (rt==(jl_value_t*)jl_float64_type){
            r = jl_box_float64((long)dcCallLong(vm, (DCpointer)fptr));
        }
        else if (jl_is_cpointer_type(rt)){
            //pointer_root[0] = jl_box_uint64((uint64_t*)argv[5]);
            //jl_(e);
            //jl_printf(JL_STDOUT,"%p",raw_ptr);
            // r = jl_new_struct((jl_datatype_t*)rt,raw_ptr);
            jl_value_t **pointer_root;
            JL_GC_PUSHARGS(pointer_root, 1);
            void* raw_ptr = dcCallPointer(vm, (DCpointer)fptr);
            jl_value_t* ptr_int = jl_box_uint64((uint64_t)raw_ptr);
            pointer_root[0] = ptr_int;
            // we need to invoke the constructor for Ptr{...} to create a Ptr pointer
            // TODO: how to directly construct a Ptr{...}?
            r = jl_new_ptr(rt,ptr_int);
            //jl_safe_printf("After jl_string_ptr:%p",raw_ptr);
            //jl_(r);
            JL_GC_POP();
        }
        else if (jl_is_abstract_ref_type(rt)){
            jl_value_t* params = jl_svec_ref(((jl_datatype_t*)rt)->parameters,0);
            if (params == ((jl_value_t*)jl_symbol_type)||(params == ((jl_value_t*)jl_module_type))){
                r = (jl_value_t*)dcCallPointer(vm, (DCpointer)fptr);
            }
            if (!jl_is_immutable_datatype(params)){
                r = (jl_value_t*)dcCallPointer(vm, (DCpointer)fptr);
            }
            else{
                //r = (jl_value_t*)dcCallPointer(vm, (DCpointer)fptr);
                jl_safe_printf("Not a Ref{Symbol}, you might be bad luck! at %s:%d\n",jl_filename,jl_lineno);
                jl_(rt);
                jl_error("Shouldn't be here!");
            };
        }
        else if (jl_is_primitivetype(rt)){
            // return type is a primitive type
            size_t size = jl_datatype_size(rt);
            const void* ptr = NULL;
            switch (size)
            {
            case 0:
                dcCallVoid(vm, (DCpointer)fptr);
                r = ((jl_datatype_t*)rt)->instance;
                break;
            case 1:
                char char_value = dcCallChar(vm, (DCpointer)fptr);
                ptr = (const void*)(&char_value);
                break;
            case 2:
                short short_value = dcCallShort(vm, (DCpointer)fptr);
                ptr = (const void*)(&short_value);
                break;
            case 4:
                int int_value = dcCallInt(vm, (DCpointer)fptr);
                ptr = (const void*)(&int_value);
                break;
            case 8:
                long long_value = dcCallLong(vm, (DCpointer)fptr);
                ptr = (const void*)(&long_value);
                break;
            case 16:
                long long long_long_value = dcCallLongLong(vm, (DCpointer)fptr);
                ptr = (const void*)(&long_long_value);
                break;
            default:
                r = (jl_value_t*)dcCallPointer(vm, (DCpointer)fptr);
                ptr = NULL;
                jl_safe_printf("Might be unsupported primitive return type, at file %s:%d \n",jl_filename,jl_lineno);
                jl_(rt);
                break;
            }
            if (ptr != NULL){
                r = (jl_value_t*)jl_new_bits(rt, ptr);
            }
        } else{
            r = (jl_value_t*)dcCallPointer(vm, (DCpointer)fptr);
            jl_safe_printf("Might be unsupported return non-primitive type, at file %s:%d \n",jl_filename,jl_lineno);
            jl_(rt);
        };
        
        dcFree(vm);
        //jl_(r);
        JL_GC_POP();
        return r;
    }
    else if (head == jl_cfunction_sym) {
        jl_value_t **argv;
        JL_GC_PUSHARGS(argv, nargs);
        for (size_t i = 0; i < nargs; i++){
            argv[i] = eval_value(args[i], s);
        }
        assert(nargs == 5);
        jl_value_t* cfuncType = argv[0];
        assert(jl_is_cpointer_type(cfuncType));
        jl_value_t* funcName = argv[1];
        // assert(jl_is_symbol(funcName));
        jl_value_t* func = eval_value(funcName, s);
        assert(func != NULL);
        jl_value_t* rt = argv[2];
        jl_value_t* inputTypes = argv[3];
        assert(jl_is_simplevector(inputTypes));
        jl_value_t* cconv = argv[4];
        assert(cconv == (jl_value_t*)jl_symbol("ccall"));
        jl_value_t* method_instances_func = jl_atomic_load_relaxed(&(jl_get_binding((jl_module_t*)jl_main_module, jl_symbol("get_all_method_instances"))->value));
        assert(method_instances_func != NULL);
        jl_value_t* input[2] = {func, inputTypes};
        jl_value_t* return_val = jl_apply_generic(method_instances_func, (jl_value_t**)&input, 2);
        assert(jl_is_array(return_val));
        assert(jl_array_len(return_val) == 1);
        jl_method_instance_t* mi = (jl_method_instance_t*)jl_ptrarrayref((jl_array_t*)return_val, 0);
        assert(jl_get_cfunction_ptr != NULL);
        void* callptr = (*jl_get_cfunction_ptr)(mi, jl_world_counter);
        jl_value_t* result = jl_new_bits(cfuncType, (const void*)&callptr);
        JL_GC_POP();
        return result;
    }
    jl_errorf("unsupported or misplaced expression %s", jl_symbol_name(head));
    abort();
}

// phi nodes don't behave like proper instructions, so we require a special interpreter to handle them
static size_t eval_phi(jl_array_t *stmts, interpreter_state *s, size_t ns, size_t to)
{
    size_t from = s->ip;
    size_t ip = to;
    unsigned nphi = 0;
    for (ip = to; ip < ns; ip++) {
        jl_value_t *e = jl_array_ptr_ref(stmts, ip);
        if (!jl_is_phinode(e))
            break;
        nphi += 1;
    }
    if (nphi) {
        jl_value_t **dest = &s->locals[jl_source_nslots(s->src) + to];
        jl_value_t **phis; // = (jl_value_t**)alloca(sizeof(jl_value_t*) * nphi);
        JL_GC_PUSHARGS(phis, nphi);
        for (unsigned i = 0; i < nphi; i++) {
            jl_value_t *e = jl_array_ptr_ref(stmts, to + i);
            assert(jl_is_phinode(e));
            jl_array_t *edges = (jl_array_t*)jl_fieldref_noalloc(e, 0);
            ssize_t edge = -1;
            size_t closest = to; // implicit edge has `to <= edge - 1 < to + i`
            // this is because we could see the following IR (all 1-indexed):
            //   goto %3 unless %cond
            //   %2 = phi ...
            //   %3 = phi (1)[1 => %a], (2)[2 => %b]
            // from = 1, to = closest = 2, i = 1 --> edge = 2, edge_from = 2, from = 2
            for (unsigned j = 0; j < jl_array_len(edges); ++j) {
                size_t edge_from = ((int32_t*)jl_array_data(edges))[j]; // 1-indexed
                if (edge_from == from + 1) {
                    if (edge == -1)
                        edge = j;
                }
                else if (closest < edge_from && edge_from < (to + i + 1)) {
                    // if we found a nearer implicit branch from fall-through,
                    // that occurred since the last explicit branch,
                    // we should use the value from that edge instead
                    edge = j;
                    closest = edge_from;
                }
            }
            jl_value_t *val = NULL;
            unsigned n_oldphi = closest - to;
            if (n_oldphi) {
                // promote this implicit branch to a basic block start
                // and move all phi values to their position in edges
                // note that we might have already processed some phi nodes
                // in this basic block, so we need to be extra careful here
                // to ignore those
                for (unsigned j = 0; j < n_oldphi; j++) {
                    dest[j] = phis[j];
                }
                for (unsigned j = n_oldphi; j < i; j++) {
                    // move the rest to the start of phis
                    phis[j - n_oldphi] = phis[j];
                    phis[j] = NULL;
                }
                from = closest - 1;
                i -= n_oldphi;
                dest += n_oldphi;
                to += n_oldphi;
                nphi -= n_oldphi;
            }
            if (edge != -1) {
                // if edges list doesn't contain last branch, or the value is explicitly undefined
                // then this value should be unused.
                jl_array_t *values = (jl_array_t*)jl_fieldref_noalloc(e, 1);
                val = jl_array_ptr_ref(values, edge);
                if (val)
                    val = eval_value(val, s);
            }
            phis[i] = val;
        }
        // now move all phi values to their position in edges
        for (unsigned j = 0; j < nphi; j++) {
            dest[j] = phis[j];
        }
        JL_GC_POP();
    }
    return ip;
}

static jl_value_t *eval_body(jl_array_t *stmts, interpreter_state *s, size_t ip, int toplevel)
{   
    //jl_safe_printf("Executing %s:%d",jl_filename,jl_lineno);
    //jl_(stmts);
    jl_handler_t __eh;
    size_t ns = jl_array_len(stmts);
    jl_task_t *ct = jl_current_task;

    while (1) {
        s->ip = ip;
        if (ip >= ns)
            jl_error("`body` expression must terminate in `return`. Use `block` instead.");
        if (toplevel)
            ct->world_age = jl_world_counter;
        jl_value_t *stmt = jl_array_ptr_ref(stmts, ip);
        assert(!jl_is_phinode(stmt));
        size_t next_ip = ip + 1;
        assert(!jl_is_phinode(stmt) && !jl_is_phicnode(stmt) && "malformed IR");
        if (jl_is_gotonode(stmt)) {
            next_ip = jl_gotonode_label(stmt) - 1;
        }
        else if (jl_is_gotoifnot(stmt)) {
            jl_value_t *cond = eval_value(jl_gotoifnot_cond(stmt), s);
            if (cond == jl_false) {
                next_ip = jl_gotoifnot_label(stmt) - 1;
            }
            else if (cond != jl_true) {
                jl_type_error("if", (jl_value_t*)jl_bool_type, cond);
            }
        }
        else if (jl_is_returnnode(stmt)) {
            return eval_value(jl_returnnode_value(stmt), s);
        }
        else if (jl_is_upsilonnode(stmt)) {
            jl_value_t *val = jl_fieldref_noalloc(stmt, 0);
            if (val)
                val = eval_value(val, s);
            jl_value_t *phic = s->locals[jl_source_nslots(s->src) + ip];
            assert(jl_is_ssavalue(phic));
            ssize_t id = ((jl_ssavalue_t*)phic)->id - 1;
            s->locals[jl_source_nslots(s->src) + id] = val;
        }
        else if (jl_is_expr(stmt)) {
            // Most exprs are allowed to end a BB by fall through
            jl_sym_t *head = ((jl_expr_t*)stmt)->head;
            if (head == jl_assign_sym) {
                jl_value_t *lhs = jl_exprarg(stmt, 0);
                jl_value_t *rhs = eval_value(jl_exprarg(stmt, 1), s);
                if (jl_is_slot(lhs)) {
                    ssize_t n = jl_slot_number(lhs);
                    assert(n <= jl_source_nslots(s->src) && n > 0);
                    s->locals[n - 1] = rhs;
                }
                else {
                    jl_module_t *modu;
                    jl_sym_t *sym;
                    if (jl_is_globalref(lhs)) {
                        modu = jl_globalref_mod(lhs);
                        sym = jl_globalref_name(lhs);
                    }
                    else {
                        assert(jl_is_symbol(lhs));
                        modu = s->module;
                        sym = (jl_sym_t*)lhs;
                    }
                    JL_GC_PUSH1(&rhs);
                    jl_binding_t *b = jl_get_binding_wr(modu, sym, 1);
                    jl_checked_assignment(b, rhs);
                    JL_GC_POP();
                }
            }
            else if (head == jl_enter_sym) {
                jl_enter_handler(&__eh);
                // This is a bit tricky, but supports the implementation of PhiC nodes.
                // They are conceptually slots, but the slot to store to doesn't get explicitly
                // mentioned in the store (aka the "UpsilonNode") (this makes them integrate more
                // nicely with the rest of the SSA representation). In a compiler, we would figure
                // out which slot to store to at compile time when we encounter the statement. We
                // can't quite do that here, but we do something similar: We scan the catch entry
                // block (the only place where PhiC nodes may occur) to find all the Upsilons we
                // can possibly encounter. Then, we remember which slot they store to (we abuse the
                // SSA value result array for this purpose). TODO: We could do this only the first
                // time we encounter a given enter.
                size_t catch_ip = jl_unbox_long(jl_exprarg(stmt, 0)) - 1;
                while (catch_ip < ns) {
                    jl_value_t *phicnode = jl_array_ptr_ref(stmts, catch_ip);
                    if (!jl_is_phicnode(phicnode))
                        break;
                    jl_array_t *values = (jl_array_t*)jl_fieldref_noalloc(phicnode, 0);
                    for (size_t i = 0; i < jl_array_len(values); ++i) {
                        jl_value_t *val = jl_array_ptr_ref(values, i);
                        assert(jl_is_ssavalue(val));
                        size_t upsilon = ((jl_ssavalue_t*)val)->id - 1;
                        assert(jl_is_upsilonnode(jl_array_ptr_ref(stmts, upsilon)));
                        s->locals[jl_source_nslots(s->src) + upsilon] = jl_box_ssavalue(catch_ip + 1);
                    }
                    s->locals[jl_source_nslots(s->src) + catch_ip] = NULL;
                    catch_ip += 1;
                }
                // store current top of exception stack for restore in pop_exception.
                s->locals[jl_source_nslots(s->src) + ip] = jl_box_ulong(jl_excstack_state());
                if (!jl_setjmp(__eh.eh_ctx, 1)) {
                    return eval_body(stmts, s, next_ip, toplevel);
                }
                else if (s->continue_at) { // means we reached a :leave expression
                    ip = s->continue_at;
                    s->continue_at = 0;
                    continue;
                }
                else { // a real exception
                    ip = catch_ip;
                    continue;
                }
            }
            else if (head == jl_leave_sym) {
                int hand_n_leave = jl_unbox_long(jl_exprarg(stmt, 0));
                assert(hand_n_leave > 0);
                // equivalent to jl_pop_handler(hand_n_leave), but retaining eh for longjmp:
                jl_handler_t *eh = ct->eh;
                while (--hand_n_leave > 0)
                    eh = eh->prev;
                jl_eh_restore_state(eh);
                // leave happens during normal control flow, but we must
                // longjmp to pop the eval_body call for each enter.
                s->continue_at = next_ip;
                jl_longjmp(eh->eh_ctx, 1);
            }
            else if (head == jl_pop_exception_sym) {
                size_t prev_state = jl_unbox_ulong(eval_value(jl_exprarg(stmt, 0), s));
                jl_restore_excstack(prev_state);
            }
            else if (toplevel) {
                if (head == jl_method_sym && jl_expr_nargs(stmt) > 1) {
                    eval_methoddef((jl_expr_t*)stmt, s);
                }
                else if (head == jl_toplevel_sym) {
                    jl_value_t *res = jl_toplevel_eval(s->module, stmt);
                    s->locals[jl_source_nslots(s->src) + s->ip] = res;
                }
                else if (jl_is_toplevel_only_expr(stmt)) {
                    jl_toplevel_eval(s->module, stmt);
                }
                else if (head == jl_meta_sym) {
                    if (jl_expr_nargs(stmt) == 1 && jl_exprarg(stmt, 0) == (jl_value_t*)jl_nospecialize_sym) {
                        jl_set_module_nospecialize(s->module, 1);
                    }
                    if (jl_expr_nargs(stmt) == 1 && jl_exprarg(stmt, 0) == (jl_value_t*)jl_specialize_sym) {
                        jl_set_module_nospecialize(s->module, 0);
                    }
                    if (jl_expr_nargs(stmt) == 2) {
                        if (jl_exprarg(stmt, 0) == (jl_value_t*)jl_optlevel_sym) {
                            if (jl_is_long(jl_exprarg(stmt, 1))) {
                                int n = jl_unbox_long(jl_exprarg(stmt, 1));
                                jl_set_module_optlevel(s->module, n);
                            }
                        }
                        else if (jl_exprarg(stmt, 0) == (jl_value_t*)jl_compile_sym) {
                            if (jl_is_long(jl_exprarg(stmt, 1))) {
                                jl_set_module_compile(s->module, jl_unbox_long(jl_exprarg(stmt, 1)));
                            }
                        }
                        else if (jl_exprarg(stmt, 0) == (jl_value_t*)jl_infer_sym) {
                            if (jl_is_long(jl_exprarg(stmt, 1))) {
                                jl_set_module_infer(s->module, jl_unbox_long(jl_exprarg(stmt, 1)));
                            }
                        }
                    }
                }
                else {
                    eval_stmt_value(stmt, s);
                }
            }
            else {
                eval_stmt_value(stmt, s);
            }
        }
        else if (jl_is_newvarnode(stmt)) {
            jl_value_t *var = jl_fieldref(stmt, 0);
            assert(jl_is_slot(var));
            ssize_t n = jl_slot_number(var);
            assert(n <= jl_source_nslots(s->src) && n > 0);
            s->locals[n - 1] = NULL;
        }
        else if (toplevel && jl_is_linenode(stmt)) {
            jl_lineno = jl_linenode_line(stmt);
        }
        else {
            eval_stmt_value(stmt, s);
        }
        ip = eval_phi(stmts, s, ns, next_ip);
    }
    abort();
}

// preparing method IR for interpreter

jl_code_info_t *jl_code_for_interpreter(jl_method_instance_t *mi)
{
    jl_code_info_t *src = (jl_code_info_t*)mi->uninferred;
    if (jl_is_method(mi->def.value)) {
        if (!src || (jl_value_t*)src == jl_nothing) {
            if (mi->def.method->source) {
                src = (jl_code_info_t*)mi->def.method->source;
            }
            else {
                assert(mi->def.method->generator);
                src = jl_code_for_staged(mi);
            }
        }
        if (src && (jl_value_t*)src != jl_nothing) {
            JL_GC_PUSH1(&src);
            src = jl_uncompress_ir(mi->def.method, NULL, (jl_array_t*)src);
            mi->uninferred = (jl_value_t*)src;
            jl_gc_wb(mi, src);
            JL_GC_POP();
        }
    }
    if (!src || !jl_is_code_info(src)) {
        jl_error("source missing for method called in interpreter");
    }
    return src;
}

// interpreter entry points

jl_value_t *NOINLINE jl_fptr_interpret_call(jl_value_t *f, jl_value_t **args, uint32_t nargs, jl_code_instance_t *codeinst)
{
    interpreter_state *s;
    jl_method_instance_t *mi = codeinst->def;
    jl_code_info_t *src = jl_code_for_interpreter(mi);
    jl_array_t *stmts = src->code;
    assert(jl_typeis(stmts, jl_array_any_type));
    unsigned nroots = jl_source_nslots(src) + jl_source_nssavalues(src) + 2;
    jl_value_t **locals = NULL;
    JL_GC_PUSHFRAME(s, locals, nroots);
    locals[0] = (jl_value_t*)src;
    locals[1] = (jl_value_t*)stmts;
    s->locals = locals + 2;
    s->src = src;
    s->jit = 0;
    int isva = 0;
    if (jl_is_module(mi->def.value)) {
        s->module = mi->def.module;
    }
    else {
        s->module = mi->def.method->module;
        size_t defargs = mi->def.method->nargs;
        isva = mi->def.method->isva ? 1 : 0;
        size_t i;
        s->locals[0] = f;
        assert(isva ? nargs + 2 >= defargs : nargs + 1 == defargs);
        for (i = 1; i < defargs - isva; i++)
            s->locals[i] = args[i - 1];
        if (isva) {
            assert(defargs >= 2);
            s->locals[defargs - 1] = jl_f_tuple(NULL, &args[defargs - 2], nargs + 2 - defargs);
        }
    }
    s->sparam_vals = mi->sparam_vals;
    s->preevaluation = 0;
    s->continue_at = 0;
    s->mi = mi;
    JL_GC_ENABLEFRAME(s);
    jl_value_t* r;
    // what's the number of args?
    // we should add one here!
    r = eval_body(stmts, s, 0, 0);
    JL_GC_POP();
    return r;
}

JL_DLLEXPORT jl_callptr_t jl_fptr_interpret_call_addr = &jl_fptr_interpret_call;

jl_value_t *jl_interpret_opaque_closure(jl_opaque_closure_t *oc, jl_value_t **args, size_t nargs)
{
    jl_method_t *source = oc->source;
    jl_code_info_t *code = jl_uncompress_ir(source, NULL, (jl_array_t*)source->source);
    interpreter_state *s;
    unsigned nroots = jl_source_nslots(code) + jl_source_nssavalues(code) + 2;
    jl_value_t **locals = NULL;
    JL_GC_PUSHFRAME(s, locals, nroots);
    locals[0] = (jl_value_t*)oc;
    // The analyzer has some trouble with this
    locals[1] = (jl_value_t*)code;
    JL_GC_PROMISE_ROOTED(code);
    locals[2] = (jl_value_t*)oc->captures;
    s->locals = locals + 2;
    s->src = code;
    s->module = source->module;
    s->sparam_vals = NULL;
    s->preevaluation = 0;
    s->continue_at = 0;
    s->mi = NULL;

    size_t defargs = source->nargs;
    int isva = !!oc->isva;
    assert(isva ? nargs + 2 >= defargs : nargs + 1 == defargs);
    for (size_t i = 1; i < defargs - isva; i++)
        s->locals[i] = args[i - 1];
    if (isva) {
        assert(defargs >= 2);
        s->locals[defargs - 1] = jl_f_tuple(NULL, &args[defargs - 2], nargs + 2 - defargs);
    }
    JL_GC_ENABLEFRAME(s);
    jl_value_t *r = eval_body(code->code, s, 0, 0);
    JL_GC_POP();
    return r;
}

jl_value_t *NOINLINE jl_interpret_toplevel_thunk_internal(jl_interp_ctx* ctx, jl_module_t *m, jl_code_info_t *src)
{
    interpreter_state *s;
    unsigned nroots = jl_source_nslots(src) + jl_source_nssavalues(src);
    JL_GC_PUSHFRAME(s, s->locals, nroots);
    jl_array_t *stmts = src->code;
    assert(jl_typeis(stmts, jl_array_any_type));
    s->src = src;
    s->module = m;
    s->sparam_vals = jl_emptysvec;
    s->continue_at = 0;
    s->mi = NULL;
    s->jit = ctx->jit;
    JL_GC_ENABLEFRAME(s);
    jl_task_t *ct = jl_current_task;
    size_t last_age = ct->world_age;
    jl_value_t *r = eval_body(stmts, s, 0, 1);
    ct->world_age = last_age;
    JL_GC_POP();
    return r;
}

jl_value_t *NOINLINE jl_interpret_toplevel_thunk(jl_module_t *m, jl_code_info_t *src){
    jl_interp_ctx ctx;
    ctx.jit = 0;
    return jl_interpret_toplevel_thunk_internal(&ctx, m, src);
}

// deprecated: do not use this method in new code
// it uses special scoping / evaluation / error rules
// which should instead be handled in lowering
jl_value_t *NOINLINE jl_interpret_toplevel_expr_in_internal(jl_interp_ctx* ctx, jl_module_t *m, jl_value_t *e, jl_code_info_t *src, jl_svec_t *sparam_vals)
{
    interpreter_state *s;
    jl_value_t **locals;
    JL_GC_PUSHFRAME(s, locals, 0);
    (void)locals;
    s->src = src;
    s->module = m;
    s->sparam_vals = sparam_vals;
    s->preevaluation = (sparam_vals != NULL);
    s->continue_at = 0;
    s->mi = NULL;
    s->jit = ctx->jit;
    JL_GC_ENABLEFRAME(s);
    jl_value_t *v = eval_value(e, s);
    assert(v);
    JL_GC_POP();
    return v;
}

jl_value_t *NOINLINE jl_interpret_toplevel_expr_in(jl_module_t *m, jl_value_t *e, jl_code_info_t *src, jl_svec_t *sparam_vals){
    jl_interp_ctx ctx;
    ctx.jit = 0;
    return jl_interpret_toplevel_expr_in_internal(&ctx, m, e, src, sparam_vals);
}

JL_DLLEXPORT size_t jl_capture_interp_frame(jl_bt_element_t *bt_entry,
        void *stateend, size_t space_remaining)
{
    interpreter_state *s = &((interpreter_state*)stateend)[-1];
    int need_module = !s->mi;
    int required_space = need_module ? 4 : 3;
    if (space_remaining < required_space)
        return 0; // Should not happen
    size_t njlvalues = need_module ? 2 : 1;
    uintptr_t entry_tags = jl_bt_entry_descriptor(njlvalues, 0, JL_BT_INTERP_FRAME_TAG, s->ip);
    bt_entry[0].uintptr = JL_BT_NON_PTR_ENTRY;
    bt_entry[1].uintptr = entry_tags;
    bt_entry[2].jlvalue = s->mi  ? (jl_value_t*)s->mi  :
                          s->src ? (jl_value_t*)s->src : (jl_value_t*)jl_nothing;
    if (need_module) {
        // If we only have a CodeInfo (s->src), we are in a top level thunk and
        // need to record the module separately.
        bt_entry[3].jlvalue = (jl_value_t*)s->module;
    }
    return required_space;
}


#ifdef __cplusplus
}
#endif
