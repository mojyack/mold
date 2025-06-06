#include "mold.h"

#include <bit>
#include <cstring>
#include <tbb/parallel_sort.h>

#ifndef _WIN32
# include <unistd.h>
#endif

namespace mold {

// If we haven't seen the same `key` before, create a new instance
// of Symbol and returns it. Otherwise, returns the previously-
// instantiated object. `key` is usually the same as `name`.
template <typename E>
Symbol<E> *get_symbol(Context<E> &ctx, std::string_view key,
                      std::string_view name) {
  typename decltype(ctx.symbol_map)::const_accessor acc;
  ctx.symbol_map.insert(acc, {key, Symbol<E>(name, ctx.arg.demangle)});
  return const_cast<Symbol<E> *>(&acc->second);
}

template <typename E>
Symbol<E> *get_symbol(Context<E> &ctx, std::string_view key) {
  std::string_view name = key.substr(0, key.find('@'));
  return get_symbol(ctx, key, name);
}

template <typename E>
static bool is_rust_symbol(const Symbol<E> &sym) {
  // The legacy Rust mangling scheme is indistinguishtable from C++.
  // We don't want to accidentally demangle C++ symbols as Rust ones.
  // So, the legacy mangling scheme will be demangled only when we
  // know the object file was created by rustc.
  if (sym.file && !sym.file->is_dso && ((ObjectFile<E> *)sym.file)->is_rust_obj)
    return true;

  // "_R" is the prefix of the new Rust mangling scheme.
  return sym.name().starts_with("_R");
}

template <typename E>
std::string_view demangle(const Symbol<E> &sym) {
  if (is_rust_symbol(sym)) {
    if (std::optional<std::string_view> s = demangle_rust(sym.name()))
      return *s;
  } else {
    if (std::optional<std::string_view> s = demangle_cpp(sym.name()))
      return *s;
  }
  return sym.name();
}

template <typename E>
std::ostream &operator<<(std::ostream &out, const Symbol<E> &sym) {
  if (sym.demangle)
    out << demangle(sym);
  else
    out << sym.name();
  return out;
}

template <typename E>
InputFile<E>::InputFile(Context<E> &ctx, MappedFile *mf)
  : mf(mf), filename(mf->name) {
  if (mf->size < sizeof(ElfEhdr<E>))
    Fatal(ctx) << *this << ": file too small";
  if (memcmp(mf->data, "\177ELF", 4))
    Fatal(ctx) << *this << ": not an ELF file";

  ElfEhdr<E> &ehdr = *(ElfEhdr<E> *)mf->data;
  is_dso = (ehdr.e_type == ET_DYN);

  ElfShdr<E> *sh_begin = (ElfShdr<E> *)(mf->data + ehdr.e_shoff);

  // e_shnum contains the total number of sections in an object file.
  // Since it is a 16-bit integer field, it's not large enough to
  // represent >65535 sections. If an object file contains more than 65535
  // sections, the actual number is stored to sh_size field.
  i64 num_sections = (ehdr.e_shnum == 0) ? sh_begin->sh_size : ehdr.e_shnum;

  if (mf->data + mf->size < (u8 *)(sh_begin + num_sections))
    Fatal(ctx) << mf->name << ": e_shoff or e_shnum corrupted: "
               << mf->size << " " << num_sections;
  elf_sections = {sh_begin, sh_begin + num_sections};

  // e_shstrndx is a 16-bit field. If .shstrtab's section index is
  // too large, the actual number is stored to sh_link field.
  i64 shstrtab_idx = (ehdr.e_shstrndx == SHN_XINDEX)
    ? sh_begin->sh_link : ehdr.e_shstrndx;

  shstrtab = this->get_string(ctx, shstrtab_idx);
}

template <typename E>
ElfShdr<E> *InputFile<E>::find_section(i64 type) {
  for (ElfShdr<E> &sec : elf_sections)
    if (sec.sh_type == type)
      return &sec;
  return nullptr;
}

// Find the source filename. It should be listed in symtab as STT_FILE.
template <typename E>
std::string_view InputFile<E>::get_source_name() const {
  for (i64 i = 0; i < first_global; i++)
    if (Symbol<E> *sym = symbols[i]; sym->get_type() == STT_FILE)
      return sym->name();
  return "";
}

template <typename E>
static bool is_debug_section(const ElfShdr<E> &shdr, std::string_view name) {
  return !(shdr.sh_flags & SHF_ALLOC) && name.starts_with(".debug");
}

template <typename E>
void
ObjectFile<E>::parse_note_gnu_property(Context<E> &ctx, const ElfShdr<E> &shdr) {
  std::string_view data = this->get_string(ctx, shdr);

  while (!data.empty()) {
    ElfNhdr<E> &hdr = *(ElfNhdr<E> *)data.data();
    data = data.substr(sizeof(hdr));

    std::string_view name = data.substr(0, hdr.n_namesz - 1);
    data = data.substr(align_to(hdr.n_namesz, 4));

    std::string_view desc = data.substr(0, hdr.n_descsz);
    data = data.substr(align_to(hdr.n_descsz, sizeof(Word<E>)));

    if (hdr.n_type != NT_GNU_PROPERTY_TYPE_0 || name != "GNU")
      continue;

    while (!desc.empty()) {
      u32 type = *(U32<E> *)desc.data();
      u32 size = *(U32<E> *)(desc.data() + 4);
      desc = desc.substr(8);

      // The majority of currently defined .note.gnu.property
      // use 32-bit values.
      // We don't know how to handle anything else, so if we encounter
      // one, skip it.
      //
      // The following properties have a different size:
      // - GNU_PROPERTY_STACK_SIZE
      // - GNU_PROPERTY_NO_COPY_ON_PROTECTED
      if (size == 4)
        gnu_properties[type] |= *(U32<E> *)desc.data();
      desc = desc.substr(align_to(size, sizeof(Word<E>)));
    }
  }
}

// <format-version>
// [ <section-length> "vendor-name" <file-tag> <size> <attribute>*]+ ]*
template <is_riscv E>
static void read_riscv_attributes(Context<E> &ctx, ObjectFile<E> &file,
                                  std::string_view data) {
  if (data.empty())
    Fatal(ctx) << file << ": corrupted .riscv.attributes section";

  if (u8 format_version = data[0]; format_version != 'A')
    return;
  data = data.substr(1);

  while (!data.empty()) {
    i64 sz = *(U32<E> *)data.data();
    if (data.size() < sz)
      Fatal(ctx) << file << ": corrupted .riscv.attributes section";

    std::string_view p(data.data() + 4, sz - 4);
    data = data.substr(sz);

    if (!p.starts_with("riscv\0"sv))
      continue;
    p = p.substr(6);

    if (!p.starts_with(ELF_TAG_FILE))
      Fatal(ctx) << file << ": corrupted .riscv.attributes section";
    p = p.substr(5); // skip the tag and the sub-sub-section size

    while (!p.empty()) {
      i64 tag = read_uleb(&p);

      switch (tag) {
      case ELF_TAG_RISCV_STACK_ALIGN:
        file.extra.stack_align = read_uleb(&p);
        break;
      case ELF_TAG_RISCV_ARCH: {
        i64 pos = p.find_first_of('\0');
        file.extra.arch = p.substr(0, pos);
        p = p.substr(pos + 1);
        break;
      }
      case ELF_TAG_RISCV_UNALIGNED_ACCESS:
        file.extra.unaligned_access = read_uleb(&p);
        break;
      default:
        break;
      }
    }
  }
}

template <typename E>
static bool is_known_section_type(const ElfShdr<E> &shdr) {
  u32 ty = shdr.sh_type;
  u32 flags = shdr.sh_flags;

  if (ty == SHT_PROGBITS ||
      ty == SHT_NOTE ||
      ty == SHT_NOBITS ||
      ty == SHT_INIT_ARRAY ||
      ty == SHT_FINI_ARRAY ||
      ty == SHT_PREINIT_ARRAY)
    return true;

  if (SHT_LOUSER <= ty && ty <= SHT_HIUSER && !(flags & SHF_ALLOC))
    return true;
  if (SHT_LOOS <= ty && ty <= SHT_HIOS && !(flags & SHF_OS_NONCONFORMING))
    return true;
  if (is_x86_64<E> && ty == SHT_X86_64_UNWIND)
    return true;
  if (is_arm32<E> && (ty == SHT_ARM_EXIDX || ty == SHT_ARM_ATTRIBUTES))
    return true;
  if (is_riscv<E> && ty == SHT_RISCV_ATTRIBUTES)
    return true;
  return false;
}

// SHT_CREL is an experimental alternative relocation table format
// designed to reduce the size of the table. Only LLVM supports it
// at the moment.
//
// This function converts a CREL relocation table to a regular one.
template <typename E>
std::vector<ElfRel<E>> decode_crel(Context<E> &ctx, ObjectFile<E> &file,
                                   const ElfShdr<E> &shdr) {
  u8 *p = (u8 *)file.get_string(ctx, shdr).data();
  u64 hdr = read_uleb(&p);
  i64 nrels = hdr >> 3;
  bool is_rela = hdr & 0b100;
  i64 scale = hdr & 0b11;

  if (is_rela && !E::is_rela)
    Fatal(ctx) << file << ": CREL with addends is not supported for " << E::name;

  u64 offset = 0;
  i64 type = 0;
  i64 symidx = 0;
  i64 addend = 0;

  std::vector<ElfRel<E>> vec;
  vec.reserve(nrels);

  while (vec.size() < nrels) {
    u8 flags = *p++;
    i64 nflags = is_rela ? 3 : 2;

    // The first ULEB-128 encoded value is a concatenation of bit flags and
    // an offset delta. The delta may be very large to decrease the
    // current offset value by wrapping around. Combined, the encoded value
    // can be up to 67 bit long. Thus we can't simply use read_uleb() which
    // returns a u64.
    u64 delta;
    if (flags & 0x80)
      delta = (read_uleb(&p) << (7 - nflags)) | ((flags & 0x7f) >> nflags);
    else
      delta = flags >> nflags;
    offset += delta << scale;

    if (flags & 1)
      symidx += read_sleb(&p);
    if (flags & 2)
      type += read_sleb(&p);
    if (is_rela && (flags & 4))
      addend += read_sleb(&p);
    vec.emplace_back(offset, type, symidx, addend);
  }
  return vec;
}

template <typename E>
ComdatGroup *insert_comdat_group(Context<E> &ctx, std::string name) {
  typename decltype(ctx.comdat_groups)::const_accessor acc;
  ctx.comdat_groups.insert(acc, {std::move(name), ComdatGroup()});
  return const_cast<ComdatGroup *>(&acc->second);
}

template <typename E>
void ObjectFile<E>::initialize_sections(Context<E> &ctx) {
  // Read sections
  for (i64 i = 0; i < this->elf_sections.size(); i++) {
    const ElfShdr<E> &shdr = this->elf_sections[i];
    std::string_view name = this->shstrtab.data() + shdr.sh_name;

    if ((shdr.sh_flags & SHF_EXCLUDE) &&
        name.starts_with(".gnu.offload_lto_.symtab.")) {
      this->is_gcc_offload_obj = true;
      continue;
    }

    if ((shdr.sh_flags & SHF_EXCLUDE) && !(shdr.sh_flags & SHF_ALLOC) &&
        shdr.sh_type != SHT_LLVM_ADDRSIG && !ctx.arg.relocatable)
      continue;

    if constexpr (is_arm<E>)
      if (shdr.sh_type == SHT_ARM_ATTRIBUTES)
        continue;

    if constexpr (is_riscv<E>) {
      if (shdr.sh_type == SHT_RISCV_ATTRIBUTES) {
        read_riscv_attributes(ctx, *this, this->get_string(ctx, shdr));
        continue;
      }
    }

    switch (shdr.sh_type) {
    case SHT_GROUP: {
      // Get the signature of this section group.
      if (shdr.sh_info >= this->elf_syms.size())
        Fatal(ctx) << *this << ": invalid symbol index";
      const ElfSym<E> &esym = this->elf_syms[shdr.sh_info];

      std::string_view signature;
      if (esym.st_type == STT_SECTION) {
        signature = this->shstrtab.data() +
                    this->elf_sections[get_shndx(esym)].sh_name;
      } else {
        signature = this->symbol_strtab.data() + esym.st_name;
      }

      // Ignore a broken comdat group GCC emits for .debug_macros.
      // https://github.com/rui314/mold/issues/438
      if (signature.starts_with("wm4."))
        continue;

      // Get comdat group members.
      std::span<U32<E>> entries = this->template get_data<U32<E>>(ctx, shdr);

      if (entries.empty())
        Fatal(ctx) << *this << ": empty SHT_GROUP";
      if (entries[0] == 0)
        continue;
      if (entries[0] != GRP_COMDAT)
        Fatal(ctx) << *this << ": unsupported SHT_GROUP format";

      ComdatGroup *group = insert_comdat_group(ctx, std::string(signature));
      comdat_groups.push_back({group, (i32)i, entries.subspan(1)});
      break;
    }
    case SHT_CREL:
      decoded_crel.resize(i + 1);
      decoded_crel[i] = decode_crel(ctx, *this, shdr);
      break;
    case SHT_REL:
    case SHT_RELA:
    case SHT_SYMTAB:
    case SHT_SYMTAB_SHNDX:
    case SHT_STRTAB:
    case SHT_NULL:
      break;
    default:
      if (!is_known_section_type(shdr))
        Fatal(ctx) << *this << ": " << name << ": unsupported section type: 0x"
                   << std::hex << (u32)shdr.sh_type;

      // .note.GNU-stack section controls executable-ness of the stack
      // area in GNU linkers. We ignore that section because silently
      // making the stack area executable is too dangerous. Tell our
      // users about the difference if that matters.
      if (name == ".note.GNU-stack" && !ctx.arg.relocatable) {
        if (shdr.sh_flags & SHF_EXECINSTR) {
          if (!ctx.arg.z_execstack && !ctx.arg.z_execstack_if_needed)
            Warn(ctx) << *this << ": this file may cause a segmentation"
              " fault because it requires an executable stack. See"
              " https://github.com/rui314/mold/tree/main/docs/execstack.md"
              " for more info.";
          needs_executable_stack = true;
        }
        continue;
      }

      if (name == ".note.gnu.property") {
        parse_note_gnu_property(ctx, shdr);
        continue;
      }

      // Ignore a build-id section in an input file. This doesn't normally
      // happen, but you can create such object file with
      // `ld.bfd -r --build-id`.
      if (name == ".note.gnu.build-id")
        continue;

      // Ignore these sections for compatibility with old glibc i386 CRT files.
      if (name == ".gnu.linkonce.t.__x86.get_pc_thunk.bx" ||
          name == ".gnu.linkonce.t.__i686.get_pc_thunk.bx")
        continue;

      // Also ignore this for compatibility with ICC
      if (name == ".gnu.linkonce.d.DW.ref.__gxx_personality_v0")
        continue;

      // Ignore debug sections if --strip-all or --strip-debug is given.
      if ((ctx.arg.strip_all || ctx.arg.strip_debug) &&
          is_debug_section(shdr, name))
        continue;

      // Ignore section is specified by --discard-section.
      if (!ctx.arg.discard_section.empty() &&
          ctx.arg.discard_section.contains(name))
        continue;

      if (name == ".comment" &&
          this->get_string(ctx, shdr).starts_with("rustc "))
        this->is_rust_obj = true;

      // If an output file doesn't have a section header (i.e.
      // --oformat=binary is given), we discard all non-memory-allocated
      // sections. This is because without a section header, we can't find
      // their places in an output file in the first place.
      if (ctx.arg.oformat_binary && !(shdr.sh_flags & SHF_ALLOC))
        continue;

      this->sections[i] = std::make_unique<InputSection<E>>(ctx, *this, i);

      // Save .llvm_addrsig for --icf=safe.
      if (shdr.sh_type == SHT_LLVM_ADDRSIG && !ctx.arg.relocatable) {
        // sh_link should be the index of the symbol table section.
        // Tools that mutates the symbol table, such as objcopy or `ld -r`
        // tend to not preserve sh_link, so we ignore such section.
        if (shdr.sh_link != 0)
          llvm_addrsig = std::move(this->sections[i]);
        continue;
      }

      if (shdr.sh_type == SHT_INIT_ARRAY ||
          shdr.sh_type == SHT_FINI_ARRAY ||
          shdr.sh_type == SHT_PREINIT_ARRAY)
        this->has_init_array = true;

      if (name == ".ctors" || name.starts_with(".ctors.") ||
          name == ".dtors" || name.starts_with(".dtors."))
        this->has_ctors = true;

      if (name == ".eh_frame")
        eh_frame_sections.push_back(this->sections[i].get());

      if constexpr (is_ppc32<E>)
        if (name == ".got2")
          extra.got2 = this->sections[i].get();

      // Save debug sections for --gdb-index.
      if (ctx.arg.gdb_index) {
        InputSection<E> *isec = this->sections[i].get();

        if (name == ".debug_info")
          debug_info = isec;

        // If --gdb-index is given, contents of .debug_gnu_pubnames and
        // .debug_gnu_pubtypes are copied to .gdb_index, so keeping them
        // in an output file is just a waste of space.
        if (name == ".debug_gnu_pubnames") {
          debug_pubnames = isec;
          isec->is_alive = false;
        }

        if (name == ".debug_gnu_pubtypes") {
          debug_pubtypes = isec;
          isec->is_alive = false;
        }

        // .debug_types is similar to .debug_info but contains type info
        // only. It exists only in DWARF 4, has been removed in DWARF 5 and
        // neither GCC nor Clang generate it by default
        // (-fdebug-types-section is needed). As such there is probably
        // little need to support it.
        if (name == ".debug_types")
          Fatal(ctx) << *this << ": mold's --gdb-index is not compatible"
            " with .debug_types; to fix this error, remove"
            " -fdebug-types-section and recompile";
      }

      static Counter counter("regular_sections");
      counter++;
      break;
    }
  }

  // Attach relocation sections to their target sections.
  for (i64 i = 0; i < this->elf_sections.size(); i++) {
    const ElfShdr<E> &shdr = this->elf_sections[i];
    if (shdr.sh_type == (E::is_rela ? SHT_RELA : SHT_REL) ||
        shdr.sh_type == SHT_CREL) {
      if (std::unique_ptr<InputSection<E>> &target = sections[shdr.sh_info]) {
        assert(target->relsec_idx == -1);
        target->relsec_idx = i;
      }
    }
  }

  // Attach .arm.exidx sections to their corresponding sections
  if constexpr (is_arm32<E>)
    for (std::unique_ptr<InputSection<E>> &isec : this->sections)
      if (isec && isec->shdr().sh_type == SHT_ARM_EXIDX)
        if (InputSection<E> *target = sections[isec->shdr().sh_link].get())
          target->extra.exidx = isec.get();
}

// .eh_frame contains data records explaining how to handle exceptions.
// When an exception is thrown, the runtime searches a record from
// .eh_frame with the current program counter as a key. A record that
// covers the current PC explains how to find a handler and how to
// transfer the control ot it.
//
// Unlike the most other sections, linker has to parse .eh_frame contents
// because of the following reasons:
//
// - There's usually only one .eh_frame section for each object file,
//   which explains how to handle exceptions for all functions in the same
//   object. If we just copy them, the resulting .eh_frame section will
//   contain lots of records for dead sections (i.e. de-duplicated inline
//   functions). We want to copy only records for live functions.
//
// - .eh_frame contains two types of records: CIE and FDE. There's usually
//   only one CIE at beginning of .eh_frame section followed by FDEs.
//   Compiler usually emits the identical CIE record for all object files.
//   We want to merge identical CIEs in an output .eh_frame section to
//   reduce the section size.
//
// - Scanning a .eh_frame section to find a record is an O(n) operation
//   where n is the number of records in the section. To reduce it to
//   O(log n), linker creates a .eh_frame_hdr section. The section
//   contains a sorted list of [an address in .text, an FDE address whose
//   coverage starts at the .text address] to make binary search doable.
//   In order to create .eh_frame_hdr, linker has to read .eh_frame.
//
// This function parses an input .eh_frame section.
template <typename E>
void ObjectFile<E>::parse_ehframe(Context<E> &ctx) {
  for (InputSection<E> *isec : eh_frame_sections) {
    std::span<ElfRel<E>> rels = isec->get_rels(ctx);
    i64 cies_begin = cies.size();
    i64 fdes_begin = fdes.size();

    // Read CIEs and FDEs until empty.
    std::string_view contents = this->get_string(ctx, isec->shdr());
    i64 rel_idx = 0;

    for (std::string_view data = contents; !data.empty();) {
      i64 size = *(U32<E> *)data.data();
      if (size == 0)
        break;

      i64 begin_offset = data.data() - contents.data();
      i64 end_offset = begin_offset + size + 4;
      i64 id = *(U32<E> *)(data.data() + 4);
      data = data.substr(size + 4);

      i64 rel_begin = rel_idx;
      while (rel_idx < rels.size() && rels[rel_idx].r_offset < end_offset)
        rel_idx++;
      assert(rel_idx == rels.size() || begin_offset <= rels[rel_begin].r_offset);

      if (id == 0) {
        // This is CIE.
        cies.emplace_back(ctx, *this, *isec, begin_offset, rels, rel_begin);
      } else {
        // This is FDE.
        if (rel_begin == rel_idx || rels[rel_begin].r_sym == 0) {
          // FDE has no valid relocation, which means FDE is dead from
          // the beginning. Compilers usually don't create such FDE, but
          // `ld -r` tend to generate such dead FDEs.
          continue;
        }

        if (rels[rel_begin].r_offset - begin_offset != 8)
          Fatal(ctx) << *isec << ": FDE's first relocation should have offset 8";

        fdes.emplace_back(begin_offset, rel_begin);
      }
    }

    // Associate CIEs to FDEs.
    auto find_cie = [&](i64 offset) {
      for (i64 i = cies_begin; i < cies.size(); i++)
        if (cies[i].input_offset == offset)
          return i;
      Fatal(ctx) << *isec << ": bad FDE pointer";
    };

    for (i64 i = fdes_begin; i < fdes.size(); i++) {
      i64 cie_offset = *(I32<E> *)(contents.data() + fdes[i].input_offset + 4);
      fdes[i].cie_idx = find_cie(fdes[i].input_offset + 4 - cie_offset);
    }

    isec->is_alive = false;
  }

  auto get_isec = [&](const FdeRecord<E> &fde) {
    return get_section(this->elf_syms[fde.get_rels(*this)[0].r_sym]);
  };

  // We assume that FDEs for the same input sections are contiguous
  // in `fdes` vector.
  ranges::stable_sort(fdes, {}, [&](const FdeRecord<E> &x) {
    return get_isec(x)->get_priority();
  });

  // Associate FDEs to input sections.
  for (i64 i = 0; i < fdes.size();) {
    InputSection<E> *isec = get_isec(fdes[i]);
    assert(isec->fde_begin == -1);

    if (isec->is_alive) {
      isec->fde_begin = i++;
      while (i < fdes.size() && isec == get_isec(fdes[i]))
        i++;
      isec->fde_end = i;
    } else {
      fdes[i++].is_alive = false;
    }
  }
}

template <typename E>
void ObjectFile<E>::initialize_symbols(Context<E> &ctx) {
  if (this->elf_syms.empty())
    return;

  static Counter counter("all_syms");
  counter += this->elf_syms.size();

  // Initialize local symbols
  this->local_syms.resize(this->first_global);
  this->local_syms[0].file = this;
  this->local_syms[0].sym_idx = 0;

  for (i64 i = 1; i < this->first_global; i++) {
    const ElfSym<E> &esym = this->elf_syms[i];
    if (esym.is_common())
      Fatal(ctx) << *this << ": common local symbol?";

    std::string_view name;
    if (esym.st_type == STT_SECTION)
      name = this->shstrtab.data() + this->elf_sections[get_shndx(esym)].sh_name;
    else
      name = this->symbol_strtab.data() + esym.st_name;

    Symbol<E> &sym = this->local_syms[i];
    sym.set_name(name);
    sym.file = this;
    sym.value = esym.st_value;
    sym.sym_idx = i;

    if (!esym.is_abs())
      sym.set_input_section(sections[get_shndx(esym)].get());
  }

  this->symbols.resize(this->elf_syms.size());

  i64 num_globals = this->elf_syms.size() - this->first_global;
  has_symver.resize(num_globals);

  for (i64 i = 0; i < this->first_global; i++)
    this->symbols[i] = &this->local_syms[i];

  // Initialize global symbols
  for (i64 i = this->first_global; i < this->elf_syms.size(); i++) {
    const ElfSym<E> &esym = this->elf_syms[i];

    if (esym.is_common())
      has_common_symbol = true;

    // Get a symbol name
    std::string_view key = this->symbol_strtab.data() + esym.st_name;
    std::string_view name = key;

    // Parse symbol version after atsign
    if (i64 pos = name.find('@'); pos != name.npos) {
      std::string_view ver = name.substr(pos);
      name = name.substr(0, pos);

      if (ver != "@" && ver != "@@") {
        if (ver.starts_with("@@"))
          key = name;
        has_symver[i - this->first_global] = true;
      }
    }

    // Handle --wrap option
    Symbol<E> *sym;
    if (esym.is_undef() && name.starts_with("__real_") &&
        ctx.arg.wrap.contains(name.substr(7))) {
      sym = get_symbol(ctx, key.substr(7), name.substr(7));
    } else {
      sym = get_symbol(ctx, key, name);
      if (esym.is_undef() && sym->is_wrapped) {
        key = save_string(ctx, "__wrap_" + std::string(key));
        name = save_string(ctx, "__wrap_" + std::string(name));
        sym = get_symbol(ctx, key, name);
      }
    }

    this->symbols[i] = sym;
  }
}

// Relocations are usually sorted by r_offset in relocation tables,
// but for some reason only RISC-V does not follow that convention.
// We expect them to be sorted, so sort them if necessary.
template <typename E>
void ObjectFile<E>::sort_relocations(Context<E> &ctx) {
  if constexpr (is_riscv<E> || is_loongarch<E>) {
    for (i64 i = 1; i < sections.size(); i++) {
      std::unique_ptr<InputSection<E>> &isec = sections[i];
      if (!isec || !isec->is_alive || !(isec->shdr().sh_flags & SHF_ALLOC))
        continue;

      std::span<ElfRel<E>> rels = isec->get_rels(ctx);
      if (!ranges::is_sorted(rels, {}, &ElfRel<E>::r_offset))
        ranges::stable_sort(rels, {}, &ElfRel<E>::r_offset);
    }
  }
}

template <typename E>
void ObjectFile<E>::convert_mergeable_sections(Context<E> &ctx) {
  // Convert InputSections to MergeableSections
  for (i64 i = 0; i < this->sections.size(); i++) {
    InputSection<E> *isec = this->sections[i].get();
    if (!isec || isec->sh_size == 0 || isec->relsec_idx != -1)
      continue;

    const ElfShdr<E> &shdr = isec->shdr();
    if (!(shdr.sh_flags & SHF_MERGE))
      continue;

    MergedSection<E> *parent =
      MergedSection<E>::get_instance(ctx, isec->name(), shdr);

    if (parent) {
      this->mergeable_sections[i] =
        std::make_unique<MergeableSection<E>>(ctx, *parent, this->sections[i]);
      this->sections[i] = nullptr;
    }
  }
}

// Usually a section is an atomic unit of inclusion or exclusion.
// Linker doesn't care about its contents. However, if a section is a
// mergeable section (a section with SHF_MERGE bit set), the linker is
// expected to split it into smaller pieces and merge each piece with
// other pieces from different object files. In mold, we call the
// atomic unit of mergeable section "section pieces".
//
// This feature is typically used for string literals. String literals
// are usually put into a mergeable section by the compiler. If the same
// string literal happens to occur in two different translation units,
// the linker merges them into a single instance of a string, so that
// the linker's output doesn't contain duplicate string literals.
//
// Handling symbols in the mergeable sections is a bit tricky. Assume
// that we have a mergeable section with the following contents and
// symbols:
//
//   Hello world\0foo bar\0
//   ^            ^
//   .rodata      .L.str1
//   .L.str0
//
// '\0' represents a NUL byte. This mergeable section contains two
// section pieces, "Hello world" and "foo bar". The first string is
// referred to by two symbols, .rodata and .L.str0, and the second by
// .L.str1. .rodata is a section symbol and therefore a local symbol
// and refers to the beginning of the section.
//
// In this example, there are actually two different ways to point to
// string "foo bar", because .rodata+12 and .L.str1+0 refer to the same
// place in the section. This kind of "out-of-bound" reference occurs
// only when a symbol is a section symbol. In other words, the compiler
// may use an offset from the beginning of a section to refer to any
// section piece in a section, but it doesn't do for any other types
// of symbols.
//
// Section garbage collection and Identical Code Folding work on graphs
// where sections or section pieces are vertices and relocations are
// edges. To make it easy to handle them, we rewrite symbols and
// relocations so that each non-absolute symbol always refers to either
// a non-mergeable section or a section piece.
//
// We do that only for SHF_ALLOC sections because GC and ICF work only
// on memory-allocated sections. Non-memory-allocated mergeable sections
// are not handled here for performance reasons.
template <typename E>
void ObjectFile<E>::reattach_section_pieces(Context<E> &ctx) {
  // Attach section pieces to symbols.
  for (i64 i = 1; i < this->elf_syms.size(); i++) {
    Symbol<E> &sym = *this->symbols[i];
    const ElfSym<E> &esym = this->elf_syms[i];

    if (esym.is_abs() || esym.is_common() || esym.is_undef())
      continue;

    i64 shndx = get_shndx(esym);
    std::unique_ptr<MergeableSection<E>> &m = mergeable_sections[shndx];
    if (!m || !m->parent.resolved)
      continue;

    SectionFragment<E> *frag;
    i64 frag_offset;
    std::tie(frag, frag_offset) = m->get_fragment(esym.st_value);

    if (!frag)
      Fatal(ctx) << *this << ": bad symbol value: " << esym.st_value;

    sym.set_frag(frag);
    sym.value = frag_offset;
  }

  // Compute the size of frag_syms.
  i64 nfrag_syms = 0;
  for (std::unique_ptr<InputSection<E>> &isec : sections)
    if (isec && (isec->shdr().sh_flags & SHF_ALLOC))
      for (ElfRel<E> &r : isec->get_rels(ctx))
        if (const ElfSym<E> &esym = this->elf_syms[r.r_sym];
            esym.st_type == STT_SECTION)
          if (mergeable_sections[get_shndx(esym)])
            nfrag_syms++;

  this->frag_syms.resize(nfrag_syms);

  // For each relocation referring to a mergeable section symbol, we
  // create a new dummy non-section symbol and redirect the relocation
  // to the newly created symbol.
  i64 idx = 0;
  for (std::unique_ptr<InputSection<E>> &isec : sections) {
    if (isec && (isec->shdr().sh_flags & SHF_ALLOC)) {
      for (ElfRel<E> &r : isec->get_rels(ctx)) {
        const ElfSym<E> &esym = this->elf_syms[r.r_sym];
        if (esym.st_type != STT_SECTION)
          continue;

        i64 shndx = get_shndx(esym);
        std::unique_ptr<MergeableSection<E>> &m = mergeable_sections[shndx];
        if (!m)
          continue;

        assert(m->parent.resolved);

        i64 r_addend = get_addend(*isec, r);
        SectionFragment<E> *frag;
        i64 in_frag_offset;
        std::tie(frag, in_frag_offset) = m->get_fragment(esym.st_value + r_addend);

        if (!frag)
          Fatal(ctx) << *this << ": bad relocation at " << r.r_sym;

        Symbol<E> &sym = this->frag_syms[idx];
        sym.file = this;
        sym.set_name("<fragment>");
        sym.sym_idx = r.r_sym;
        sym.visibility = STV_HIDDEN;
        sym.set_frag(frag);
        sym.value = in_frag_offset - r_addend;
        r.r_sym = this->elf_syms.size() + idx;
        idx++;
      }
    }
  }

  assert(idx == this->frag_syms.size());

  for (Symbol<E> &sym : this->frag_syms)
    this->symbols.push_back(&sym);
}

template <typename E>
void ObjectFile<E>::parse(Context<E> &ctx) {
  sections.resize(this->elf_sections.size());
  mergeable_sections.resize(sections.size());

  symtab_sec = this->find_section(SHT_SYMTAB);

  if (symtab_sec) {
    // In ELF, all local symbols precede global symbols in the symbol table.
    // sh_info has an index of the first global symbol.
    this->first_global = symtab_sec->sh_info;
    this->elf_syms = this->template get_data<ElfSym<E>>(ctx, *symtab_sec);
    this->symbol_strtab = this->get_string(ctx, symtab_sec->sh_link);

    if (ElfShdr<E> *shdr = this->find_section(SHT_SYMTAB_SHNDX))
      symtab_shndx_sec = this->template get_data<U32<E>>(ctx, *shdr);
  }

  initialize_sections(ctx);
  initialize_symbols(ctx);
  sort_relocations(ctx);
}

// Symbols with higher priorities overwrites symbols with lower priorities.
// Here is the list of priorities, from the highest to the lowest.
//
//  1. Strong defined symbol
//  2. Weak defined symbol
//  3. Strong defined symbol in a DSO/archive
//  4. Weak Defined symbol in a DSO/archive
//  5. Common symbol
//  6. Common symbol in an archive
//  7. Unclaimed (nonexistent) symbol
//
// Ties are broken by file priority.
//
// Note that the above priorities are based on heuristics and not on exact
// science. We tried several different orders and settled on the current
// one just because it avoids link errors in all programs we've tested.
template <typename E>
static u64 get_rank(InputFile<E> *file, const ElfSym<E> &esym, bool is_in_archive) {
  auto get_sym_rank = [&] {
    if (esym.is_common()) {
      assert(!file->is_dso);
      return is_in_archive ? 6 : 5;
    }

    if (file->is_dso || is_in_archive)
      return (esym.st_bind == STB_WEAK) ? 4 : 3;

    if (esym.st_bind == STB_WEAK)
      return 2;
    return 1;
  };

  return (get_sym_rank() << 24) + file->priority;
}

template <typename E>
static u64 get_rank(const Symbol<E> &sym) {
  if (!sym.file)
    return 7 << 24;
  return get_rank(sym.file, sym.esym(), !sym.file->is_reachable);
}

// Symbol's visibility is set to the most restrictive one. For example,
// if one input file has a defined symbol `foo` with the default
// visibility and the other input file has an undefined symbol `foo`
// with the hidden visibility, the resulting symbol is a hidden defined
// symbol.
template <typename E>
void ObjectFile<E>::merge_visibility(Context<E> &ctx, Symbol<E> &sym,
                                     u8 visibility) {
  // Canonicalize visibility
  if (visibility == STV_INTERNAL)
    visibility = STV_HIDDEN;

  auto priority = [&](u8 visibility) {
    switch (visibility) {
    case STV_HIDDEN:
      return 1;
    case STV_PROTECTED:
      return 2;
    case STV_DEFAULT:
      return 3;
    }
    Fatal(ctx) << *this << ": unknown symbol visibility: " << sym;
  };

  update_minimum(sym.visibility, visibility, [&](u8 a, u8 b) {
    return priority(a) < priority(b);
  });
}

template <typename E>
static void print_trace_symbol(Context<E> &ctx, InputFile<E> &file,
                               const ElfSym<E> &esym, Symbol<E> &sym) {
  if (!esym.is_undef())
    Out(ctx) << "trace-symbol: " << file << ": definition of " << sym;
  else if (esym.is_weak())
    Out(ctx) << "trace-symbol: " << file << ": weak reference to " << sym;
  else
    Out(ctx) << "trace-symbol: " << file << ": reference to " << sym;
}

template <typename E>
void ObjectFile<E>::resolve_symbols(Context<E> &ctx) {
  for (i64 i = this->first_global; i < this->elf_syms.size(); i++) {
    Symbol<E> &sym = *this->symbols[i];
    const ElfSym<E> &esym = this->elf_syms[i];

    if (esym.is_undef())
      continue;

    InputSection<E> *isec = nullptr;
    if (!esym.is_abs() && !esym.is_common()) {
      isec = get_section(esym);
      if (!isec || !isec->is_alive)
        continue;
    }

    std::scoped_lock lock(sym.mu);

    if (get_rank(this, esym, !this->is_reachable) < get_rank(sym)) {
      sym.file = this;
      sym.set_input_section(isec);
      sym.value = esym.st_value;
      sym.sym_idx = i;
      sym.ver_idx = ctx.default_version;
      sym.is_weak = esym.is_weak();
      sym.is_versioned_default = false;
    }
  }
}

template <typename E>
void
ObjectFile<E>::mark_live_objects(Context<E> &ctx,
                                 std::function<void(InputFile<E> *)> feeder) {
  assert(this->is_reachable);

  for (i64 i = this->first_global; i < this->elf_syms.size(); i++) {
    const ElfSym<E> &esym = this->elf_syms[i];
    Symbol<E> &sym = *this->symbols[i];

    if (!esym.is_undef() && exclude_libs)
      merge_visibility(ctx, sym, STV_HIDDEN);
    else
      merge_visibility(ctx, sym, esym.st_visibility);

    if (sym.is_traced)
      print_trace_symbol(ctx, *this, esym, sym);

    if (sym.file) {
      bool undef_ref = esym.is_undef() && (!esym.is_weak() || sym.file->is_dso);
      bool common_ref = esym.is_common() && !sym.esym().is_common();

      if ((undef_ref || common_ref) && !sym.file->is_reachable.test_and_set()) {
        feeder(sym.file);
        if (sym.is_traced)
          Out(ctx) << "trace-symbol: " << *this << " keeps " << *sym.file
                   << " for " << sym;
      }
    }
  }
}

template <typename E>
void ObjectFile<E>::scan_relocations(Context<E> &ctx) {
  // Scan relocations against seciton contents
  for (std::unique_ptr<InputSection<E>> &isec : sections)
    if (isec && isec->is_alive && (isec->shdr().sh_flags & SHF_ALLOC))
      isec->scan_relocations(ctx);

  // Scan relocations against exception frames
  for (CieRecord<E> &cie : cies) {
    for (ElfRel<E> &rel : cie.get_rels()) {
      Symbol<E> &sym = *this->symbols[rel.r_sym];

      if (ctx.arg.pic && rel.r_type == E::R_ABS)
        Error(ctx) << *this << ": relocation " << rel << " in .eh_frame can"
                   << " not be used when making a position-independent output;"
                   << " recompile with -fPIE or -fPIC";

      if (sym.is_imported) {
        if (sym.get_type() != STT_FUNC)
          Fatal(ctx) << *this << ": " << sym
                     << ": .eh_frame CIE record with an external data reference"
                     << " is not supported";
        sym.flags |= NEEDS_PLT;
      }
    }
  }
}

// Common symbols are used by C's tantative definitions. Tentative
// definition is an obscure C feature which allows users to omit `extern`
// from global variable declarations in a header file. For example, if you
// have a tentative definition `int foo;` in a header which is included
// into multiple translation units, `foo` will be included into multiple
// object files, but it won't cause the duplicate symbol error. Instead,
// the linker will merge them into a single instance of `foo`.
//
// If a header file contains a tentative definition `int foo;` and one of
// a C file contains a definition with initial value such as `int foo = 5;`,
// then the "real" definition wins. The symbol for the tentative definition
// will be resolved to the real definition. If there is no "real"
// definition, the tentative definition gets the default initial value 0.
//
// Tentative definitions are represented as "common symbols" in an object
// file. In this function, we allocate spaces in .common or .tls_common
// for remaining common symbols that were not resolved to usual defined
// symbols in previous passes.
template <typename E>
void ObjectFile<E>::convert_common_symbols(Context<E> &ctx) {
  if (!has_common_symbol)
    return;

  for (i64 i = this->first_global; i < this->elf_syms.size(); i++) {
    if (!this->elf_syms[i].is_common())
      continue;

    Symbol<E> &sym = *this->symbols[i];
    if (sym.file != this) {
      if (ctx.arg.warn_common)
        Warn(ctx) << *this << ": multiple common symbols: " << sym;
      continue;
    }

    ElfShdr<E> shdr = {};
    if (sym.get_type() == STT_TLS)
      shdr.sh_flags = SHF_ALLOC | SHF_WRITE | SHF_TLS;
    else
      shdr.sh_flags = SHF_ALLOC | SHF_WRITE;

    shdr.sh_type = SHT_NOBITS;
    shdr.sh_size = this->elf_syms[i].st_size;
    shdr.sh_addralign = this->elf_syms[i].st_value;
    elf_sections2.push_back(shdr);

    i64 idx = this->elf_sections.size() + elf_sections2.size() - 1;
    auto isec = std::make_unique<InputSection<E>>(ctx, *this, idx);

    sym.set_input_section(isec.get());
    sym.value = 0;
    sym.sym_idx = i;
    sym.ver_idx = ctx.default_version;
    sym.is_weak = false;
    sections.push_back(std::move(isec));
  }
}

template <typename E>
static bool should_write_to_local_symtab(Context<E> &ctx, Symbol<E> &sym) {
  if (sym.get_type() == STT_SECTION)
    return false;

  // Local symbols are discarded if --discard-local is given or they
  // are in a mergeable section. I *believe* we exclude symbols in
  // mergeable sections because (1) there are too many and (2) they are
  // merged, so their origins shouldn't matter, but I don't really
  // know the rationale. Anyway, this is the behavior of the
  // traditional linkers.
  if (sym.name().starts_with(".L") || sym.name() == "L0\001") {
    if (ctx.arg.discard_locals)
      return false;

    if (InputSection<E> *isec = sym.get_input_section())
      if (isec->shdr().sh_flags & SHF_MERGE)
        return false;
  }

  return true;
}

template <typename E>
void ObjectFile<E>::compute_symtab_size(Context<E> &ctx) {
  this->output_sym_indices.resize(this->elf_syms.size(), -1);

  auto is_alive = [](Symbol<E> &sym) -> bool {
    if (SectionFragment<E> *frag = sym.get_frag())
      return frag->is_alive;
    if (InputSection<E> *isec = sym.get_input_section())
      return isec->is_alive;
    return true;
  };

  // Compute the size of local symbols
  if (!ctx.arg.discard_all && !ctx.arg.strip_all && !ctx.arg.retain_symbols_file) {
    for (i64 i = 1; i < this->first_global; i++) {
      Symbol<E> &sym = *this->symbols[i];

      if (is_alive(sym) && should_write_to_local_symtab(ctx, sym)) {
        this->strtab_size += sym.name().size() + 1;
        this->output_sym_indices[i] = this->num_local_symtab++;
        sym.write_to_symtab = true;
      }
    }
  }

  // Compute the size of global symbols.
  for (i64 i = this->first_global; i < this->elf_syms.size(); i++) {
    Symbol<E> &sym = *this->symbols[i];

    if (sym.file == this && is_alive(sym) &&
        (!ctx.arg.retain_symbols_file || sym.write_to_symtab)) {
      this->strtab_size += sym.name().size() + 1;
      // Global symbols can be demoted to local symbols based on visibility,
      // version scripts etc.
      if (sym.is_local(ctx))
        this->output_sym_indices[i] = this->num_local_symtab++;
      else
        this->output_sym_indices[i] = this->num_global_symtab++;
      sym.write_to_symtab = true;
    }
  }
}

template <typename E>
void ObjectFile<E>::populate_symtab(Context<E> &ctx) {
  ElfSym<E> *symtab_base = (ElfSym<E> *)(ctx.buf + ctx.symtab->shdr.sh_offset);

  u8 *strtab_base = ctx.buf + ctx.strtab->shdr.sh_offset;
  i64 strtab_off = this->strtab_offset;

  auto write_sym = [&](Symbol<E> &sym, i64 idx) {
    U32<E> *xindex = nullptr;
    if (ctx.symtab_shndx)
      xindex = (U32<E> *)(ctx.buf + ctx.symtab_shndx->shdr.sh_offset) + idx;

    symtab_base[idx] = to_output_esym(ctx, sym, strtab_off, xindex);
    strtab_off += write_string(strtab_base + strtab_off, sym.name());
  };

  i64 local_idx = this->local_symtab_idx;
  i64 global_idx = this->global_symtab_idx;

  for (i64 i = 1; i < this->first_global; i++)
    if (Symbol<E> &sym = *this->symbols[i]; sym.write_to_symtab)
      write_sym(sym, local_idx++);

  for (i64 i = this->first_global; i < this->elf_syms.size(); i++) {
    Symbol<E> &sym = *this->symbols[i];
    if (sym.file == this && sym.write_to_symtab)
      write_sym(sym, sym.is_local(ctx) ? local_idx++ : global_idx++);
  }
}

template <typename E>
std::ostream &operator<<(std::ostream &out, const InputFile<E> &file) {
  if (file.is_dso) {
    out << path_clean(file.filename);
    return out;
  }

  ObjectFile<E> *obj = (ObjectFile<E> *)&file;
  if (obj->archive_name == "")
    out << path_clean(obj->filename);
  else
    out << path_clean(obj->archive_name) << "(" << obj->filename + ")";
  return out;
}

template <typename E>
std::string SharedFile<E>::get_soname(Context<E> &ctx) {
  if (ElfShdr<E> *sec = this->find_section(SHT_DYNAMIC))
    for (ElfDyn<E> &dyn : this->template get_data<ElfDyn<E>>(ctx, *sec))
      if (dyn.d_tag == DT_SONAME)
        return this->get_string(ctx, sec->sh_link).data() + dyn.d_val;

  if (this->mf->given_fullpath)
    return this->filename;

  return path_filename(this->filename);
}

template <typename E>
void SharedFile<E>::parse(Context<E> &ctx) {
  symtab_sec = this->find_section(SHT_DYNSYM);
  if (!symtab_sec)
    return;

  this->symbol_strtab = this->get_string(ctx, symtab_sec->sh_link);
  soname = get_soname(ctx);
  version_strings = read_verdef(ctx);

  // Read a symbol table.
  std::span<ElfSym<E>> esyms = this->template get_data<ElfSym<E>>(ctx, *symtab_sec);

  std::span<U16<E>> vers;
  if (ElfShdr<E> *sec = this->find_section(SHT_GNU_VERSYM))
    vers = this->template get_data<U16<E>>(ctx, *sec);

  for (i64 i = symtab_sec->sh_info; i < esyms.size(); i++) {
    u16 ver;
    if (vers.empty() || esyms[i].is_undef())
      ver = VER_NDX_GLOBAL;
    else
      ver = vers[i] & ~VERSYM_HIDDEN;

    if (ver == VER_NDX_LOCAL)
      continue;

    this->elf_syms2.push_back(esyms[i]);
    this->versyms.push_back(ver);

    std::string_view name = this->symbol_strtab.data() + esyms[i].st_name;

    auto get_versioned_sym = [&] {
      std::string_view key = save_string(
        ctx, std::string(name) + "@" + std::string(version_strings[ver]));
      return get_symbol(ctx, key, name);
    };

    // Symbol resolution involving symbol versioning is tricky because one
    // symbol can be resolved with two different identifiers. Among
    // symbols with the same name but different versions, one of them is
    // always marked as the "default" one. This symbol is often denoted
    // with two atsigns as `foo@@VERSION` and can be referred to either
    // as `foo` or `foo@VERSION`. No other symbols have two names like that.
    //
    // On contrary, a versioned non-default symbol can be referred only
    // with an explicit version suffix, e.g., `foo@VERSION`.
    //
    // Here is how we resolve versioned default symbols. We resolve `foo`
    // and `foo@VERSION` as usual, but with information to forward
    // references to `foo@VERSION` to `foo`. After name resolution, we
    // visit all symbol references to redirect `foo@VERSION` to `foo`.
    if (vers.empty() || ver == VER_NDX_GLOBAL) {
      // Unversioned symbol
      this->symbols.push_back(get_symbol(ctx, name));
      this->symbols2.push_back(nullptr);
    } else if (vers[i] & VERSYM_HIDDEN) {
      // Versioned non-default symbol
      this->symbols.push_back(get_versioned_sym());
      this->symbols2.push_back(nullptr);
    } else {
      // Versioned default symbol
      this->symbols.push_back(get_symbol(ctx, name));
      this->symbols2.push_back(get_versioned_sym());
    }
  }

  this->elf_syms = elf_syms2;
  this->first_global = 0;

  static Counter counter("dso_syms");
  counter += this->elf_syms.size();
}

template <typename E>
std::vector<std::string_view> SharedFile<E>::get_dt_needed(Context<E> &ctx) {
  std::vector<std::string_view> vec;
  if (ElfShdr<E> *sec = this->find_section(SHT_DYNAMIC))
    for (ElfDyn<E> &dyn : this->template get_data<ElfDyn<E>>(ctx, *sec))
      if (dyn.d_tag == DT_NEEDED)
        vec.push_back(this->get_string(ctx, sec->sh_link).data() + dyn.d_val);
  return vec;
}

template <typename E>
std::string_view SharedFile<E>::get_dt_audit(Context<E> &ctx) {
  if (ElfShdr<E> *sec = this->find_section(SHT_DYNAMIC))
    for (ElfDyn<E> &dyn : this->template get_data<ElfDyn<E>>(ctx, *sec))
      if (dyn.d_tag == DT_AUDIT)
        return this->get_string(ctx, sec->sh_link).data() + dyn.d_val;
  return "";
}

// Symbol versioning is a GNU extension to the ELF file format. I don't
// particularly like the feature as it complicates the semantics of
// dynamic linking, but we need to support it anyway because it is
// mandatory on glibc-based systems such as most Linux distros.
//
// Let me explain what symbol versioning is. Symbol versioning is a
// mechanism to allow multiple symbols of the same name but of different
// versions live together in a shared object file. It's convenient if you
// want to make an API-breaking change to some function but want to keep
// old programs working with the newer libraries.
//
// With symbol versioning, dynamic symbols are resolved by (name, version)
// tuple instead of just by name. For example, glibc 2.35 defines two
// different versions of `posix_spawn`, `posix_spawn` of version
// "GLIBC_2.15" and that of version "GLIBC_2.2.5". Any executable that
// uses `posix_spawn` is linked either to that of "GLIBC_2.15" or that of
// "GLIBC_2.2.5"
//
// Versions are just strings, and no ordering is defined between them.
// For example, "GLIBC_2.15" is not considered a newer version of
// "GLIBC_2.2.5" or vice versa. They are considered just different.
//
// If a shared object file has versioned symbols, it contains a parallel
// array for the symbol table. Version strings can be found in that
// parallel table.
//
// One version is considered the "default" version for each shared object.
// If an undefiend symbol `foo` is resolved to a symbol defined by the
// shared object, it's marked so that it'll be resolved to (`foo`, the
// default version of the library) at load-time.
template <typename E>
std::vector<std::string_view> SharedFile<E>::read_verdef(Context<E> &ctx) {
  ElfShdr<E> *verdef_sec = this->find_section(SHT_GNU_VERDEF);
  if (!verdef_sec)
    return {};

  std::string_view verdef = this->get_string(ctx, *verdef_sec);
  std::string_view strtab = this->get_string(ctx, verdef_sec->sh_link);

  std::vector<std::string_view> vec;
  u8 *ptr = (u8 *)verdef.data();

  for (;;) {
    ElfVerdef<E> *ver = (ElfVerdef<E> *)ptr;
    if (ver->vd_ndx == VER_NDX_UNSPECIFIED)
      Fatal(ctx) << *this << ": symbol version too large";

    if (vec.size() <= ver->vd_ndx)
      vec.resize(ver->vd_ndx + 1);

    ElfVerdaux<E> *aux = (ElfVerdaux<E> *)(ptr + ver->vd_aux);
    vec[ver->vd_ndx] = strtab.data() + aux->vda_name;
    if (!ver->vd_next)
      break;
    ptr += ver->vd_next;
  }
  return vec;
}

template <typename E>
void SharedFile<E>::resolve_symbols(Context<E> &ctx) {
  for (i64 i = 0; i < this->symbols.size(); i++) {
    Symbol<E> &sym = *this->symbols[i];
    const ElfSym<E> &esym = this->elf_syms[i];

    if (esym.is_undef() || sym.skip_dso)
      continue;

    std::scoped_lock lock(sym.mu);

    if (get_rank(this, esym, false) < get_rank(sym)) {
      sym.file = this;
      sym.origin = 0;
      sym.value = esym.st_value;
      sym.sym_idx = i;
      sym.ver_idx = versyms[i];
      sym.is_weak = true;
      sym.is_versioned_default = false;
    }

    // A symbol with the default version is a special case because, unlike
    // other symbols, the symbol can be referred by two names, `foo` and
    // `foo@VERSION`. Here, we resolve `foo@VERSOIN` as a proxy of `foo`.
    Symbol<E> *sym2 = this->symbols2[i];
    if (sym2 && sym2 != &sym) {
      std::scoped_lock lock2(sym2->mu);

      if (get_rank(this, esym, false) < get_rank(*sym2)) {
        sym2->file = this;
        sym2->origin = (uintptr_t)&sym;
        sym2->sym_idx = i;
        sym2->is_versioned_default = true;
      }
    }
  }
}

template <typename E>
void
SharedFile<E>::mark_live_objects(Context<E> &ctx,
                                 std::function<void(InputFile<E> *)> feeder) {
  for (i64 i = 0; i < this->elf_syms.size(); i++) {
    const ElfSym<E> &esym = this->elf_syms[i];
    Symbol<E> &sym = *this->symbols[i];

    if (sym.is_traced)
      print_trace_symbol(ctx, *this, esym, sym);

    // We follow undefined symbols in a DSO only to handle
    // --no-allow-shlib-undefined.
    if (esym.is_undef() && !esym.is_weak() && sym.file &&
        (!sym.file->is_dso || !ctx.arg.allow_shlib_undefined) &&
        !sym.file->is_reachable.test_and_set()) {
      feeder(sym.file);

      if (sym.is_traced)
        Out(ctx) << "trace-symbol: " << *this << " keeps " << *sym.file
                 << " for " << sym;
    }
  }
}

template <typename E>
std::span<Symbol<E> *> SharedFile<E>::get_symbols_at(Symbol<E> *sym) {
  assert(sym->file == this);

  std::call_once(init_sorted_syms, [&] {
    for (Symbol<E> *sym : this->symbols)
      if (sym->file == this)
        sorted_syms.push_back(sym);

    tbb::parallel_sort(sorted_syms.begin(), sorted_syms.end(),
                       [](Symbol<E> *a, Symbol<E> *b) {
      const ElfSym<E> &x = a->esym();
      const ElfSym<E> &y = b->esym();
      return std::tuple{x.st_value, &x} < std::tuple{y.st_value, &y};
    });
  });

  auto [begin, end] = ranges::equal_range(sorted_syms, sym->esym().st_value, {},
                                          [](Symbol<E> *x) {
    return x->esym().st_value;
  });
  return {&*begin, (size_t)(end - begin)};
}

// Infer an alignment of a DSO symbol. An alignment of a symbol in other
// .so is not something we usually care about, but when we create a copy
// relocation for a symbol, we need to preserve its alignment requirement.
//
// Symbol alignment is not explicitly represented in an ELF file. In this
// function, we conservatively infer it from a symbol address and a
// section alignment requirement.
template <typename E>
i64 SharedFile<E>::get_alignment(Symbol<E> *sym) {
  ElfShdr<E> &shdr = this->elf_sections[sym->esym().st_shndx];
  i64 align = std::max<i64>(1, shdr.sh_addralign);
  if (sym->value)
    align = std::min<i64>(align, 1LL << std::countr_zero(sym->value));
  return align;
}

template <typename E>
bool SharedFile<E>::is_readonly(Symbol<E> *sym) {
  ElfEhdr<E> &ehdr = *(ElfEhdr<E> *)this->mf->data;
  std::span<ElfPhdr<E>> phdrs((ElfPhdr<E> *)(this->mf->data + ehdr.e_phoff),
                              ehdr.e_phnum);
  u64 val = sym->esym().st_value;

  for (ElfPhdr<E> &phdr : phdrs)
    if ((phdr.p_type == PT_LOAD || phdr.p_type == PT_GNU_RELRO) &&
        !(phdr.p_flags & PF_W) &&
        phdr.p_vaddr <= val && val < phdr.p_vaddr + phdr.p_memsz)
      return true;
  return false;
}

template <typename E>
void SharedFile<E>::compute_symtab_size(Context<E> &ctx) {
  this->output_sym_indices.resize(this->elf_syms.size(), -1);

  // Compute the size of global symbols.
  for (i64 i = this->first_global; i < this->symbols.size(); i++) {
    Symbol<E> &sym = *this->symbols[i];

    if (sym.file == this && (sym.is_imported || sym.is_exported) &&
        (!ctx.arg.retain_symbols_file || sym.write_to_symtab)) {
      this->strtab_size += sym.name().size() + 1;
      this->output_sym_indices[i] = this->num_global_symtab++;
      sym.write_to_symtab = true;
    }
  }
}

template <typename E>
void SharedFile<E>::populate_symtab(Context<E> &ctx) {
  ElfSym<E> *symtab =
    (ElfSym<E> *)(ctx.buf + ctx.symtab->shdr.sh_offset) + this->global_symtab_idx;

  u8 *strtab = ctx.buf + ctx.strtab->shdr.sh_offset;
  i64 strtab_off = this->strtab_offset;

  for (i64 i = 0; Symbol<E> *sym : this->get_global_syms()) {
    if (sym->file != this || !sym->write_to_symtab)
      continue;

    U32<E> *xindex = nullptr;
    if (ctx.symtab_shndx)
      xindex = (U32<E> *)(ctx.buf + ctx.symtab_shndx->shdr.sh_offset) +
               this->global_symtab_idx + i;

    *symtab++ = to_output_esym(ctx, *sym, strtab_off, xindex);
    strtab_off += write_string(strtab + strtab_off, sym->name());
    i++;
  }
}

using E = MOLD_TARGET;

template class InputFile<E>;
template class ObjectFile<E>;
template class SharedFile<E>;
template Symbol<E> *get_symbol(Context<E> &, std::string_view, std::string_view);
template Symbol<E> *get_symbol(Context<E> &, std::string_view);
template std::string_view demangle(const Symbol<E> &);
template ComdatGroup *insert_comdat_group(Context<E> &, std::string);
template std::ostream &operator<<(std::ostream &, const Symbol<E> &);
template std::ostream &operator<<(std::ostream &, const InputFile<E> &);

} // namespace mold
