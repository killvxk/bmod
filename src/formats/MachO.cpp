#include <QFile>
#include <QDebug>

#include <cmath>

#include "MachO.h"
#include "../Util.h"
#include "../Reader.h"

MachO::MachO(const QString &file) : Format(FormatType::MachO), file{file} { }

bool MachO::detect() {
  QFile f{file};
  if (!f.open(QIODevice::ReadOnly)) {
    return false;
  }

  Reader r(f);
  bool ok;
  quint32 magic = r.getUInt32(&ok);
  if (!ok) return false;

  return magic == 0xFEEDFACE || // 32-bit little endian
    magic == 0xFEEDFACF || // 64-bit little endian
    magic == 0xECAFDEEF || // 32-bit big endian
    magic == 0xFCAFDEEF || // 64-bit big endian
    magic == 0xCAFEBABE || // Universal binary little endian
    magic == 0xBEBAFECA; // Universal binary big endian
}

bool MachO::parse() {
  QFile f{file};
  if (!f.open(QIODevice::ReadOnly)) {
    return false;
  }

  Reader r(f);
  bool ok;
  quint32 magic = r.getUInt32(&ok);
  if (!ok) return false;

  // Check if this is a universal "fat" binary.
  if (magic == 0xCAFEBABE || magic == 0xBEBAFECA) {
    // Values are saved as big-endian so read as such.
    r.setLittleEndian(false);

    quint32 nfat_arch = r.getUInt32(&ok);
    if (!ok) return false;

    // Read "fat" headers.
    typedef QPair<quint32, quint32> puu;
    QList<puu> archs;
    for (quint32 i = 0; i < nfat_arch; i++) {
      // CPU type.
      r.getUInt32(&ok);
      if (!ok) return false;

      // CPU sub type.
      r.getUInt32(&ok);
      if (!ok) return false;

      // File offset to this object file.
      quint32 offset = r.getUInt32(&ok);
      if (!ok) return false;

      // Size of this object file.
      quint32 size = r.getUInt32(&ok);
      if (!ok) return false;

      // Alignment as a power of 2.
      r.getUInt32(&ok);
      if (!ok) return false;

      archs << puu(offset, size);
    }

    // Parse the actual binary objects.
    foreach (const auto &arch, archs) {
      if (!parseHeader(arch.first, arch.second, r)) {
        return false;
      }
    }
  }

  // Otherwise, just parse a single object file.
  else {
    return parseHeader(0, 0, r);
  }

  return true;
}

bool MachO::parseHeader(quint32 offset, quint32 size, Reader &r) {
  BinaryObjectPtr binaryObject(new BinaryObject);

  r.seek(offset);
  r.setLittleEndian(true);

  bool ok;
  quint32 magic = r.getUInt32(&ok);
  if (!ok) return false;

  int systemBits{32};
  bool littleEndian{true};
  if (magic == 0xFEEDFACE) {
    systemBits = 32;
    littleEndian = true;
  }
  else if (magic == 0xFEEDFACF) {
    systemBits = 64;
    littleEndian = true;
  }
  else if (magic == 0xECAFDEEF) {
    systemBits = 32;
    littleEndian = false;
  }
  else if (magic == 0xFCAFDEEF) {
    systemBits = 64;
    littleEndian = false;
  }

  binaryObject->setSystemBits(systemBits);
  binaryObject->setLittleEndian(littleEndian);

  // Read info in the endianness of the file.
  r.setLittleEndian(littleEndian);

  quint32 cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags;

  cputype = r.getUInt32(&ok);
  if (!ok) return false;

  cpusubtype = r.getUInt32(&ok);
  if (!ok) return false;

  filetype = r.getUInt32(&ok);
  if (!ok) return false;

  ncmds = r.getUInt32(&ok);
  if (!ok) return false;

  sizeofcmds = r.getUInt32(&ok);
  if (!ok) return false;

  flags = r.getUInt32(&ok);
  if (!ok) return false;

  // Read reserved field.
  if (systemBits == 64) {
    r.getUInt32();
  }

  // Types in /usr/local/mach/machine.h
  CpuType cpuType{CpuType::X86};
  if (cputype == 7) { // CPU_TYPE_X86, CPU_TYPE_I386
    cpuType = CpuType::X86;
  }
  else if (cputype == 7 + 0x01000000) { // CPU_TYPE_X86 | CPU_ARCH_ABI64
    cpuType = CpuType::X86_64;
  }
  else if (cputype == 11) { // CPU_TYPE_HPPA
    cpuType = CpuType::HPPA;
  }
  else if (cputype == 12) { // CPU_TYPE_ARM
    cpuType = CpuType::ARM;
  }
  else if (cputype == 14) { // CPU_TYPE_SPARC
    cpuType = CpuType::SPARC;
  }
  else if (cputype == 15) { // CPU_TYPE_I860
    cpuType = CpuType::I860;
  }
  else if (cputype == 18) { // CPU_TYPE_POWERPC
    cpuType = CpuType::PowerPC;
  }
  else if (cputype == 18 + 0x01000000) { // CPU_TYPE_POWERPC | CPU_ARCH_ABI64
    cpuType = CpuType::PowerPC_64;
  }

  binaryObject->setCpuType(cpuType);

  // Subtract 64-bit mask.
  if (systemBits == 64) {
    cpusubtype -= 0x80000000;
  }

  CpuType cpuSubType{CpuType::I386};
  if (cpusubtype == 3) { // CPU_SUBTYPE_386
    cpuSubType = CpuType::I386;
  }
  else if (cpusubtype == 4) { // CPU_SUBTYPE_486
    cpuSubType = CpuType::I486;
  }
  else if (cpusubtype == 4 + (8 << 4)) { // CPU_SUBTYPE_486SX
    cpuSubType = CpuType::I486_SX;
  }
  else if (cpusubtype == 5) { // CPU_SUBTYPE_PENT
    cpuSubType = CpuType::Pentium;
  }
  else if (cpusubtype == 6 + (1 << 4)) { // CPU_SUBTYPE_PENTPRO
    cpuSubType = CpuType::PentiumPro;
  }
  else if (cpusubtype == 6 + (3 << 4)) { // CPU_SUBTYPE_PENTII_M3
    cpuSubType = CpuType::PentiumII_M3;
  }
  else if (cpusubtype == 6 + (5 << 4)) { // CPU_SUBTYPE_PENTII_M5
    cpuSubType = CpuType::PentiumII_M5;
  }
  else if (cpusubtype == 7 + (6 << 4)) { // CPU_SUBTYPE_CELERON
    cpuSubType = CpuType::Celeron;
  }
  else if (cpusubtype == 7 + (7 << 4)) { // CPU_SUBTYPE_CELERON_MOBILE
    cpuSubType = CpuType::CeleronMobile;
  }
  else if (cpusubtype == 8) { // CPU_SUBTYPE_PENTIUM_3
    cpuSubType = CpuType::Pentium_3;
  }
  else if (cpusubtype == 8 + (1 << 4)) { // CPU_SUBTYPE_PENTIUM_3_M
    cpuSubType = CpuType::Pentium_3_M;
  }
  else if (cpusubtype == 8 + (2 << 4)) { // CPU_SUBTYPE_PENTIUM_3_XEON
    cpuSubType = CpuType::Pentium_3_Xeon;
  }
  else if (cpusubtype == 9) { // CPU_SUBTYPE_PENTIUM_M
    cpuSubType = CpuType::Pentium_M;
  }
  else if (cpusubtype == 10) { // CPU_SUBTYPE_PENTIUM_4
    cpuSubType = CpuType::Pentium_4;
  }
  else if (cpusubtype == 10 + (1 << 4)) { // CPU_SUBTYPE_PENTIUM_4_M
    cpuSubType = CpuType::Pentium_4_M;
  }
  else if (cpusubtype == 11) { // CPU_SUBTYPE_ITANIUM
    cpuSubType = CpuType::Itanium;
  }
  else if (cpusubtype == 11 + (1 << 4)) { // CPU_SUBTYPE_ITANIUM_2
    cpuSubType = CpuType::Itanium_2;
  }
  else if (cpusubtype == 12) { // CPU_SUBTYPE_XEON
    cpuSubType = CpuType::Xeon;
  }
  else if (cpusubtype == 12 + (1 << 4)) { // CPU_SUBTYPE_XEON_MP
    cpuSubType = CpuType::Xeon_MP;
  }

  binaryObject->setCpuSubType(cpuSubType);

  FileType fileType{FileType::Object};
  if (filetype == 1) { // MH_OBJECT
    fileType = FileType::Object;
  }
  else if (filetype == 2) { // MH_EXECUTE
    fileType = FileType::Execute;
  }
  else if (filetype == 4) { // MH_CORE
    fileType = FileType::Core;
  }
  else if (filetype == 5) { // MH_PRELOAD
    fileType = FileType::Preload;
  }
  else if (filetype == 6) { // MH_DYLIB
    fileType = FileType::Dylib;
  }
  else if (filetype == 7) { // MH_DYLINKER
    fileType = FileType::Dylinker;
  }
  else if (filetype == 8) { // MH_BUNDLE
    fileType = FileType::Bundle;
  }

  binaryObject->setFileType(fileType);

  // TODO: Load flags when necessary.

  // Symbol table offset and number of elements in it.
  quint32 symoff{0}, symnum{0};

  // Dynamic (indirect) symbol table offset and number of elements in
  // it.
  quint32 indirsymoff{0}, indirsymnum{0};

  // Parse load commands sequentially. Each consists of the type, size
  // and data.
  for (int i = 0; i < ncmds; i++) {
    quint32 type = r.getUInt32(&ok);
    if (!ok) return false;

    quint32 cmdsize = r.getUInt32(&ok);
    if (!ok) return false;

    // LC_SEGMENT or LC_SEGMENT_64
    if (type == 1 || type == 25) {
      QString name{r.read(16)};

      // Memory address of this segment.
      quint64 vmaddr;
      if (systemBits == 32) {
        vmaddr = r.getUInt32(&ok);
        if (!ok) return false;
      }
      else {
        vmaddr = r.getUInt64(&ok);
        if (!ok) return false;
      }

      // Memory size of this segment.
      quint64 vmsize;
      if (systemBits == 32) {
        vmsize = r.getUInt32(&ok);
        if (!ok) return false;
      }
      else {
        vmsize = r.getUInt64(&ok);
        if (!ok) return false;
      }

      // File offset of this segment.
      quint64 fileoff;
      if (systemBits == 32) {
        fileoff = r.getUInt32(&ok);
        if (!ok) return false;
      }
      else {
        fileoff = r.getUInt64(&ok);
        if (!ok) return false;
      }

      // Amount to map from the file.
      quint64 filesize;
      if (systemBits == 32) {
        filesize = r.getUInt32(&ok);
        if (!ok) return false;
      }
      else {
        filesize = r.getUInt64(&ok);
        if (!ok) return false;
      }

      // Maximum VM protection.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Initial VM protection.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Number of sections in segment.
      quint32 nsects = r.getUInt32(&ok);
      if (!ok) return false;

      // Flags.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Read sections.
      if (nsects > 0) {
        for (int j = 0; j < nsects; j++) {
          QString secname{r.read(16)};

          QString segname{r.read(16)};

          // Memory address of this section.
          quint64 addr;
          if (systemBits == 32) {
            addr = r.getUInt32(&ok);
            if (!ok) return false;
          }
          else {
            addr = r.getUInt64(&ok);
            if (!ok) return false;
          }

          // Size in bytes of this section.
          quint64 secsize;
          if (systemBits == 32) {
            secsize = r.getUInt32(&ok);
            if (!ok) return false;
          }
          else {
            secsize = r.getUInt64(&ok);
            if (!ok) return false;
          }

          // File offset of this section.
          quint32 secfileoff = r.getUInt32(&ok);
          if (!ok) return false;

          // Section alignment (power of 2).
          r.getUInt32(&ok);
          if (!ok) return false;

          // File offset of relocation entries.
          r.getUInt32(&ok);
          if (!ok) return false;

          // Number of relocation entries.
          r.getUInt32(&ok);
          if (!ok) return false;

          // Flags.
          r.getUInt32(&ok);
          if (!ok) return false;

          // Reserved fields.
          r.getUInt32();
          r.getUInt32();
          if (systemBits == 64) {
            r.getUInt32();
          }

          // Store needed sections.
          if (segname == "__TEXT") {
            if (secname == "__text") {
              SectionPtr sec(new Section(SectionType::Text,
                                         QObject::tr("Program"),
                                         addr, secsize, offset + secfileoff));
              binaryObject->addSection(sec);
            }
            else if (secname == "__symbol_stub" ||
                     secname == "__stubs") {
              SectionPtr sec(new Section(SectionType::SymbolStubs,
                                         QObject::tr("Symbol Stubs"),
                                         addr, secsize, offset + secfileoff));
              binaryObject->addSection(sec);
            }
            else if (secname == "__cstring") {
              SectionPtr sec(new Section(SectionType::CString,
                                         QObject::tr("C-Strings"),
                                         addr, secsize, offset + secfileoff));
              binaryObject->addSection(sec);
            }
            else if (secname == "__objc_methname") {
              SectionPtr sec(new Section(SectionType::CString,
                                         QObject::tr("ObjC Method Names"),
                                         addr, secsize, offset + secfileoff));
              binaryObject->addSection(sec);
            }
          }

        }
      }
    }

    // LC_DYLD_INFO or LC_DYLD_INFO_ONLY
    else if (type == 0x22 || type == (0x22 | 0x80000000)) {
      // File offset to rebase info.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Size of rebase info.
      r.getUInt32(&ok);
      if (!ok) return false;

      // File offset to binding info.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Size of binding info.
      r.getUInt32(&ok);
      if (!ok) return false;

      // File offset to weak binding info.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Size of weak binding info.
      r.getUInt32(&ok);
      if (!ok) return false;

      // File offset to lazy binding info.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Size of lazy binding info.
      r.getUInt32(&ok);
      if (!ok) return false;

      // File offset to export info.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Size of export info.
      r.getUInt32(&ok);
      if (!ok) return false;
    }

    // LC_SYMTAB
    else if (type == 2) {
      // Symbol table offset.
      symoff = r.getUInt32(&ok);
      if (!ok) return false;

      // Number of symbol table entries.
      symnum = r.getUInt32(&ok);
      if (!ok) return false;

      // String table offset.
      quint32 stroff = r.getUInt32(&ok);
      if (!ok) return false;

      // String table size in bytes.
      quint32 strsize = r.getUInt32(&ok);
      if (!ok) return false;

      SectionPtr sec(new Section(SectionType::String,
                                 QObject::tr("String Table"),
                                 stroff, strsize, offset + stroff));
      binaryObject->addSection(sec);
    }

    // LC_DYSYMTAB
    else if (type == 0xB) {
      // Index to local symbols.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Number of local symbols.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Index to externally defined symbols.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Number of externally defined symbols.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Index to undefined defined symbols.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Number of undefined defined symbols.
      r.getUInt32(&ok);
      if (!ok) return false;

      // File offset to table of contents.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Number of entries in the table of contents.
      r.getUInt32(&ok);
      if (!ok) return false;

      // File offset to module table.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Number of module table entries.
      r.getUInt32(&ok);
      if (!ok) return false;

      // File offset to referenced symbol table.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Number of referenced symbol table entries.
      r.getUInt32(&ok);
      if (!ok) return false;

      // File offset to indirect symbol table.
      indirsymoff = r.getUInt32(&ok);
      if (!ok) return false;

      // Number of indirect symbol table entries.
      indirsymnum = r.getUInt32(&ok);
      if (!ok) return false;

      // File offset to external relocation entries.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Number of external relocation entries.
      r.getUInt32(&ok);
      if (!ok) return false;

      // File offset to local relocation entries.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Number of local relocation entries.
      r.getUInt32(&ok);
      if (!ok) return false;
    }

    // LC_LOAD_DYLIB, LC_ID_DYLIB or LC_LOAD_WEAK_DYLIB
    else if (type == 0xC || type == 0xD || type == 0x18 + 0x80000000) {
      // Library path name offset.
      quint32 liboffset = r.getUInt32(&ok);
      if (!ok) return false;

      // Time stamp.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Current version.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Compatibility version.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Library path name.
      r.read(cmdsize - liboffset);
    }

    // LC_LOAD_DYLINKER or LC_DYLD_ENVIRONMENT
    else if (type == 0xE || type == 0x27) {
      // Dynamic linker's path name.
      quint32 noffset = r.getUInt32(&ok);
      if (!ok) return false;

      r.read(cmdsize - noffset);
    }

    // LC_UUID
    else if (type == 0x1B) {
      r.read(16);
    }

    // LC_VERSION_MIN_MACOSX
    else if (type == 0x24) {
      // Version (X.Y.Z is encoded in nibbles xxxx.yy.zz)
      r.getUInt32(&ok);
      if (!ok) return false;

      // SDK version (X.Y.Z is encoded in nibbles xxxx.yy.zz)
      r.getUInt32(&ok);
      if (!ok) return false;
    }

    // LC_SOURCE_VERSION
    else if (type == 0x2A) {
      // Version (A.B.C.D.E packed as a24.b10.c10.d10.e10)
      r.getUInt64(&ok);
      if (!ok) return false;
    }

    // LC_MAIN
    else if (type == (0x28 | 0x80000000)) {
      // File (__TEXT) offset of main()
      r.getUInt64(&ok);
      if (!ok) return false;

      // Initial stack size if not zero.
      r.getUInt64(&ok);
      if (!ok) return false;
    }

    // LC_FUNCTION_STARTS, LC_DYLIB_CODE_SIGN_DRS,
    // LC_SEGMENT_SPLIT_INFO or LC_CODE_SIGNATURE
    else if (type == 0x26 || type == 0x2B || type == 0x1E || type == 0x1D) {
      // File offset to data in __LINKEDIT segment.
      quint32 off = r.getUInt32(&ok);
      if (!ok) return false;

      // File size of data in __LINKEDIT segment.
      quint32 siz = r.getUInt32(&ok);
      if (!ok) return false;

      // LC_FUNCTION_STARTS
      if (type == 0x26) {
        SectionPtr sec(new Section(SectionType::FuncStarts,
                                   QObject::tr("Function Starts"),
                                   off, siz, offset + off));
        binaryObject->addSection(sec);
      }

      // LC_CODE_SIGNATURE
      else if (type == 0x1D) {
        SectionPtr sec(new Section(SectionType::CodeSig,
                                   QObject::tr("Code Signature"),
                                   off, siz, offset + off));
        binaryObject->addSection(sec);
      }
    }

    // LC_DATA_IN_CODE
    else if (type == 0x29) {
      // From mach_header to start of data range.
      r.getUInt32(&ok);
      if (!ok) return false;

      // Number of bytes in data range.
      r.getUInt16(&ok);
      if (!ok) return false;

      // Dice kind value.
      r.getUInt16(&ok);
      if (!ok) return false;
    }

    // LC_THREAD or LC_UNIXTHREAD
    else if (type == 0x4 || type == 0x5) {
      quint32 flavor = r.getUInt32(&ok);
      if (!ok) return false;

      quint32 count = r.getUInt32(&ok);
      if (!ok) return false;

      // Data.
      r.read(flavor * count);
    }

    // LC_RPATH
    else if (type == 0x1C + 0x80000000) {
      // Name offset.
      quint32 off = r.getUInt32(&ok);
      if (!ok) return false;

      // Name.
      r.read(cmdsize - off);
    }

    // Temporary: Fail if unknown!
    else {
      qDebug() << "what is type:" << type;
      exit(0);
    }
  }

  // Parse symbol table if found.
  // (/usr/include/macho/nlist.h)
  quint32 symsize{0};
  SymbolTable symTable;
  if (symnum > 0) {
    r.seek(symoff);
    qint64 pos;
    for (int i = 0; i < symnum; i++) {
      pos = r.pos();

      // Index into the string table.
      quint32 index = r.getUInt32(&ok);
      if (!ok) return false;

      // Type flag.
      r.getUChar(&ok);
      if (!ok) return false;

      // Section number or NO_SECT.
      r.getUChar(&ok);
      if (!ok) return false;

      // Description.
      r.getUInt16(&ok);
      if (!ok) return false;

      // Value of the symbol (or stab offset).
      quint64 value;
      if (systemBits == 32) {
        value = r.getUInt32(&ok);
        if (!ok) return false;
      }
      else {
        value = r.getUInt64(&ok);
        if (!ok) return false;
      }

      symTable.addSymbol(SymbolEntry(index, value));
      symsize += (r.pos() - pos);
    }

    SectionPtr sec(new Section(SectionType::Symbols,
                               QObject::tr("Symbol Table"),
                               symoff, symsize, offset + symoff));
    binaryObject->addSection(sec);
  }

  // Parse dynamic symbol table if found. Store the offsets into the
  // symbol table for later updating.
  quint32 dynsymsize{0};
  SymbolTable dynsymTable;
  if (indirsymnum > 0) {
    r.seek(indirsymoff);
    qint64 pos;
    for (int i = 0; i < indirsymnum; i++) {
      pos = r.pos();

      quint32 num = r.getUInt32(&ok);
      if (!ok) return false;

      dynsymTable.addSymbol(SymbolEntry(num, 0));
      dynsymsize += (r.pos() - pos);
    }

    SectionPtr sec(new Section(SectionType::DynSymbols,
                               QObject::tr("Dynamic Symbol Table"),
                               indirsymoff, dynsymsize, offset + indirsymoff));
    binaryObject->addSection(sec);
  }

  // Fill data of stored sections.
  foreach (auto sec, binaryObject->getSections()) {
    r.seek(sec->getOffset());
    sec->setData(r.read(sec->getSize()));
  }

  // If symbol table loaded then merge string table entries into it.
  if (symnum > 0) {
    auto strTable = binaryObject->getSection(SectionType::String);
    if (strTable) {
      auto &data = strTable->getData();
      auto &symbols = symTable.getSymbols();
      for (int h = 0; h < symbols.size(); h++) {
        auto &symbol = symbols[h];
        QByteArray tmp;
        for (int i = symbol.getIndex(); i < data.size(); i++) {
          char c = data[i];
          tmp += c;
          if (c == 0) break;
        }
        symbol.setString(QString::fromUtf8(tmp));
      }
    }
    binaryObject->setSymbolTable(symTable);
  }

  // If dynamic symbol table loaded then merge data from symbol table
  // and symbol stubs into it.
  if (indirsymnum > 0 && symnum > 0) {
    auto stubs = binaryObject->getSection(SectionType::SymbolStubs);
    if (stubs) {
      quint64 stubAddr = stubs->getAddress();
      const auto &symbols = symTable.getSymbols();
      auto &dynsymbols = dynsymTable.getSymbols();
      for (int h = 0; h < dynsymbols.size(); h++) {
        auto &symbol = dynsymbols[h];
        int idx = symbol.getIndex();
        if (idx >= 0 && idx < symnum) {
          // The index corresponds to the index in the symbol table.
          symbol.setString(symbols[idx].getString());

          // Each symbol stub takes up 6 bytes.
          symbol.setValue(stubAddr + h * 6);
        }
      }
    }
    binaryObject->setDynSymbolTable(dynsymTable);
  }

  objects << binaryObject;
  return true;
}
