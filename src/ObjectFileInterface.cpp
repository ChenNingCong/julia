//===------ ObjectFileInterface.cpp - MU interface utils for objects ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ObjectFileInterface.h"
// #include "ELFNixPlatform.h"
// #include "MachOPlatform.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Debug.h"
using llvm::StringRef;
bool MachOPlatform_isInitializerSection(llvm::StringRef SegName,
                                        llvm::StringRef SectName) {
    StringRef EHFrameSectionName = "__TEXT,__eh_frame";
    StringRef ModInitFuncSectionName = "__DATA,__mod_init_func";
    StringRef ObjCClassListSectionName = "__DATA,__objc_classlist";
    StringRef ObjCImageInfoSectionName = "__DATA,__objc_image_info";
    StringRef ObjCSelRefsSectionName = "__DATA,__objc_selrefs";
    StringRef Swift5ProtoSectionName = "__TEXT,__swift5_proto";
    StringRef Swift5ProtosSectionName = "__TEXT,__swift5_protos";
    StringRef Swift5TypesSectionName = "__TEXT,__swift5_types";
    StringRef ThreadBSSSectionName = "__DATA,__thread_bss";
    StringRef ThreadDataSectionName = "__DATA,__thread_data";
    StringRef ThreadVarsSectionName = "__DATA,__thread_vars";
  
    StringRef InitSectionNames[] = {
        ModInitFuncSectionName, ObjCSelRefsSectionName, ObjCClassListSectionName,
        Swift5ProtosSectionName, Swift5ProtoSectionName, Swift5TypesSectionName};

    for (auto &Name : InitSectionNames) {
        if (Name.startswith(SegName) && Name.substr(7) == SectName)
            return true;
    }
    return false;
}

bool ELFNixPlatform_isInitializerSection(llvm::StringRef SecName) {
    StringRef InitArrayFuncSectionName = ".init_array";
    if (SecName.consume_front(InitArrayFuncSectionName) &&
        (SecName.empty() || SecName[0] == '.'))
        return true;
    return false;
}
  


#define DEBUG_TYPE "orc"

namespace llvm {
namespace orc {

void addInitSymbol(Interface &I, ExecutionSession &ES,
                   StringRef ObjFileName) {
  assert(!I.InitSymbol && "I already has an init symbol");
  size_t Counter = 0;

  do {
    std::string InitSymString;
    raw_string_ostream(InitSymString)
        << "$." << ObjFileName << ".__inits." << Counter++;
    I.InitSymbol = ES.intern(InitSymString);
  } while (I.SymbolFlags.count(I.InitSymbol));

  I.SymbolFlags[I.InitSymbol] = JITSymbolFlags::MaterializationSideEffectsOnly;
}

static Expected<Interface>
getMachOObjectFileSymbolInfo(ExecutionSession &ES,
                             const object::MachOObjectFile &Obj) {
  Interface I;

  for (auto &Sym : Obj.symbols()) {
    Expected<uint32_t> SymFlagsOrErr = Sym.getFlags();
    if (!SymFlagsOrErr)
      // TODO: Test this error.
      return SymFlagsOrErr.takeError();

    // Skip symbols not defined in this object file.
    if (*SymFlagsOrErr & object::BasicSymbolRef::SF_Undefined)
      continue;

    // Skip symbols that are not global.
    if (!(*SymFlagsOrErr & object::BasicSymbolRef::SF_Global))
      continue;

    // Skip symbols that have type SF_File.
    if (auto SymType = Sym.getType()) {
      if (*SymType == object::SymbolRef::ST_File)
        continue;
    } else
      return SymType.takeError();

    auto Name = Sym.getName();
    if (!Name)
      return Name.takeError();
    auto SymFlags = JITSymbolFlags::fromObjectSymbol(Sym);
    if (!SymFlags)
      return SymFlags.takeError();

    // Strip the 'exported' flag from MachO linker-private symbols.
    if (Name->startswith("l"))
      *SymFlags &= ~JITSymbolFlags::Exported;

    I.SymbolFlags[ES.intern(*Name)] = std::move(*SymFlags);
  }

  for (auto &Sec : Obj.sections()) {
    auto SecType = Obj.getSectionType(Sec);
    if ((SecType & MachO::SECTION_TYPE) == MachO::S_MOD_INIT_FUNC_POINTERS) {
      addInitSymbol(I, ES, Obj.getFileName());
      break;
    }
    auto SegName = Obj.getSectionFinalSegmentName(Sec.getRawDataRefImpl());
    auto SecName = cantFail(Obj.getSectionName(Sec.getRawDataRefImpl()));
    if (MachOPlatform_isInitializerSection(SegName, SecName)) {
      addInitSymbol(I, ES, Obj.getFileName());
      break;
    }
  }

  return I;
}

static Expected<Interface>
getELFObjectFileSymbolInfo(ExecutionSession &ES,
                           const object::ELFObjectFileBase &Obj) {
  Interface I;

  for (auto &Sym : Obj.symbols()) {
    Expected<uint32_t> SymFlagsOrErr = Sym.getFlags();
    if (!SymFlagsOrErr)
      // TODO: Test this error.
      return SymFlagsOrErr.takeError();

    // Skip symbols not defined in this object file.
    if (*SymFlagsOrErr & object::BasicSymbolRef::SF_Undefined)
      continue;

    // Skip symbols that are not global.
    if (!(*SymFlagsOrErr & object::BasicSymbolRef::SF_Global))
      continue;

    // Skip symbols that have type SF_File.
    if (auto SymType = Sym.getType()) {
      if (*SymType == object::SymbolRef::ST_File)
        continue;
    } else
      return SymType.takeError();

    auto Name = Sym.getName();
    if (!Name)
      return Name.takeError();

    auto SymFlags = JITSymbolFlags::fromObjectSymbol(Sym);
    if (!SymFlags)
      return SymFlags.takeError();

    // ELF STB_GNU_UNIQUE should map to Weak for ORC.
    if (Sym.getBinding() == ELF::STB_GNU_UNIQUE)
      *SymFlags |= JITSymbolFlags::Weak;

    I.SymbolFlags[ES.intern(*Name)] = std::move(*SymFlags);
  }

  SymbolStringPtr InitSymbol;
  for (auto &Sec : Obj.sections()) {
    if (auto SecName = Sec.getName()) {
      if (ELFNixPlatform_isInitializerSection(*SecName)) {
        addInitSymbol(I, ES, Obj.getFileName());
        break;
      }
    }
  }

  return I;
}

static Expected<Interface>
getCOFFObjectFileSymbolInfo(ExecutionSession &ES,
                            const object::COFFObjectFile &Obj) {
  Interface I;
  std::vector<Optional<object::coff_aux_section_definition>> ComdatDefs(
      Obj.getNumberOfSections() + 1);
  for (auto &Sym : Obj.symbols()) {
    Expected<uint32_t> SymFlagsOrErr = Sym.getFlags();
    if (!SymFlagsOrErr)
      // TODO: Test this error.
      return SymFlagsOrErr.takeError();

    // Handle comdat symbols
    auto COFFSym = Obj.getCOFFSymbol(Sym);
    bool IsWeak = false;
    if (auto *Def = COFFSym.getSectionDefinition()) {
      auto Sec = Obj.getSection(COFFSym.getSectionNumber());
      if (!Sec)
        return Sec.takeError();
      if (((*Sec)->Characteristics & COFF::IMAGE_SCN_LNK_COMDAT) &&
          Def->Selection != COFF::IMAGE_COMDAT_SELECT_ASSOCIATIVE) {
        ComdatDefs[COFFSym.getSectionNumber()] = *Def;
        continue;
      }
    }
    if (!COFF::isReservedSectionNumber(COFFSym.getSectionNumber()) &&
        ComdatDefs[COFFSym.getSectionNumber()]) {
      auto Def = ComdatDefs[COFFSym.getSectionNumber()];
      if (Def->Selection != COFF::IMAGE_COMDAT_SELECT_NODUPLICATES) {
        IsWeak = true;
      }
      ComdatDefs[COFFSym.getSectionNumber()] = None;
    } else {
      // Skip symbols not defined in this object file.
      if (*SymFlagsOrErr & object::BasicSymbolRef::SF_Undefined)
        continue;
    }

    // Skip symbols that are not global.
    if (!(*SymFlagsOrErr & object::BasicSymbolRef::SF_Global))
      continue;

    // Skip symbols that have type SF_File.
    if (auto SymType = Sym.getType()) {
      if (*SymType == object::SymbolRef::ST_File)
        continue;
    } else
      return SymType.takeError();

    auto Name = Sym.getName();
    if (!Name)
      return Name.takeError();

    auto SymFlags = JITSymbolFlags::fromObjectSymbol(Sym);
    if (!SymFlags)
      return SymFlags.takeError();
    *SymFlags |= JITSymbolFlags::Exported;

    // Weak external is always a function
    if (COFFSym.isWeakExternal())
      *SymFlags |= JITSymbolFlags::Callable;

    if (IsWeak)
      *SymFlags |= JITSymbolFlags::Weak;

    I.SymbolFlags[ES.intern(*Name)] = std::move(*SymFlags);
  }

  // FIXME: handle init symbols

  return I;
}

Expected<Interface>
getGenericObjectFileSymbolInfo(ExecutionSession &ES,
                               const object::ObjectFile &Obj) {
  Interface I;

  for (auto &Sym : Obj.symbols()) {
    Expected<uint32_t> SymFlagsOrErr = Sym.getFlags();
    if (!SymFlagsOrErr)
      // TODO: Test this error.
      return SymFlagsOrErr.takeError();

    // Skip symbols not defined in this object file.
    if (*SymFlagsOrErr & object::BasicSymbolRef::SF_Undefined)
      continue;

    // Skip symbols that are not global.
    if (!(*SymFlagsOrErr & object::BasicSymbolRef::SF_Global))
      continue;

    // Skip symbols that have type SF_File.
    if (auto SymType = Sym.getType()) {
      if (*SymType == object::SymbolRef::ST_File)
        continue;
    } else
      return SymType.takeError();

    auto Name = Sym.getName();
    if (!Name)
      return Name.takeError();

    auto SymFlags = JITSymbolFlags::fromObjectSymbol(Sym);
    if (!SymFlags)
      return SymFlags.takeError();

    I.SymbolFlags[ES.intern(*Name)] = std::move(*SymFlags);
  }

  return I;
}

Expected<Interface>
getObjectFileInterface(ExecutionSession &ES, MemoryBufferRef ObjBuffer) {
  auto Obj = object::ObjectFile::createObjectFile(ObjBuffer);

  if (!Obj)
    return Obj.takeError();

  if (auto *MachOObj = dyn_cast<object::MachOObjectFile>(Obj->get()))
    return getMachOObjectFileSymbolInfo(ES, *MachOObj);
  else if (auto *ELFObj = dyn_cast<object::ELFObjectFileBase>(Obj->get()))
    return getELFObjectFileSymbolInfo(ES, *ELFObj);
  else if (auto *COFFObj = dyn_cast<object::COFFObjectFile>(Obj->get()))
    return getCOFFObjectFileSymbolInfo(ES, *COFFObj);

  return getGenericObjectFileSymbolInfo(ES, **Obj);
}

} // End namespace orc.
} // End namespace llvm.
