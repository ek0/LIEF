/* Copyright 2017 R. Thomas
 * Copyright 2017 Quarkslab
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <regex>
#include <fstream>
#include <iterator>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <functional>

#include "easylogging++.h"

#include "LIEF/filesystem/filesystem.h"
#include "LIEF/exception.hpp"

#include "LIEF/ELF/Parser.hpp"
#include "LIEF/ELF/utils.hpp"


#include "Parser.tcc"


namespace LIEF {
namespace ELF {


Parser::~Parser(void) = default;
Parser::Parser(void)  = default;

Parser::Parser(const std::vector<uint8_t>& data, const std::string& name, DYNSYM_COUNT_METHODS count_mtd) :
  stream_{std::unique_ptr<VectorStream>(new VectorStream{data})},
  binary_{nullptr},
  type_{0},
  count_mtd_{count_mtd}
{
  this->init(name);
}

Parser::Parser(const std::string& file, DYNSYM_COUNT_METHODS count_mtd) :
  LIEF::Parser{file},
  binary_{nullptr},
  type_{0},
  count_mtd_{count_mtd}
{
  if (not is_elf(file)) {
    throw LIEF::bad_format("'" + file + "' is not an ELF");
  }

  this->stream_ = std::unique_ptr<VectorStream>(new VectorStream{file});
  this->init(filesystem::path(file).filename());
}

void Parser::init(const std::string& name) {
  LOG(DEBUG) << "Parsing binary: " << name << std::endl;
  this->binary_ = new Binary{};
  this->binary_->original_size_ = this->binary_size_;
  this->binary_->name(name);
  this->binary_->datahandler_ = new DataHandler::Handler{this->stream_->content()};

  this->type_ = reinterpret_cast<const Elf32_Ehdr*>(
      this->stream_->read(0, sizeof(Elf32_Ehdr)))->e_ident[IDENTITY::EI_CLASS];

  this->binary_->type_ = static_cast<ELF_CLASS>(this->type_);

  switch (this->binary_->type_) {
    case ELFCLASS32:
      {
        this->parse_binary<ELF32>();
        break;
      }

    case ELFCLASS64:
      {
        this->parse_binary<ELF64>();
        break;
      }

    case ELFCLASSNONE:
    default:
      LOG(WARNING) << "e_ident[EI_CLASS] seems corrupted." << std::endl;
      //TODO try to guess with e_machine
      throw LIEF::corrupted("e_ident[EI_CLASS] corrupted");
  }
}

Binary* Parser::parse(const std::string& filename, DYNSYM_COUNT_METHODS count_mtd) {
  Parser parser{filename, count_mtd};
  return parser.binary_;
}

Binary* Parser::parse(
    const std::vector<uint8_t>& data,
    const std::string& name,
    DYNSYM_COUNT_METHODS count_mtd) {
  Parser parser{data, name, count_mtd};
  return parser.binary_;
}


void Parser::parse_symbol_version(uint64_t symbol_version_offset) {
  LOG(DEBUG) << "[+] Parsing symbol version" << std::endl;

  LOG(DEBUG) << "Symbol version offset: 0x" << std::hex << symbol_version_offset << std::endl;

  const uint32_t nb_entries = static_cast<uint32_t>(this->binary_->dynamic_symbols_.size());
  const uint16_t* array = reinterpret_cast<const uint16_t*>(
      this->stream_->read(symbol_version_offset, nb_entries * sizeof(uint16_t)));

  for (size_t i = 0; i < nb_entries; ++i) {
    this->binary_->symbol_version_table_.push_back(new SymbolVersion{array[i]});
  }
}


uint64_t Parser::get_dynamic_string_table_from_segments(void) const {
  //find DYNAMIC segment
  auto&& it_segment_dynamic = std::find_if(
      std::begin(this->binary_->segments_),
      std::end(this->binary_->segments_),
      [] (const Segment* segment)
      {
        return segment != nullptr and segment->type() == SEGMENT_TYPES::PT_DYNAMIC;
      });


  uint64_t va_offset = 0;
  if (it_segment_dynamic != std::end(this->binary_->segments_)) {
    uint64_t offset = (*it_segment_dynamic)->file_offset();
    uint64_t size   = (*it_segment_dynamic)->physical_size();

    if (this->type_ == ELFCLASS32) {

      size_t nb_entries = size / sizeof(Elf32_Dyn);
      const Elf32_Dyn* entries = reinterpret_cast<const Elf32_Dyn*>(
          this->stream_->read(offset, size));
      for (size_t i = 0; i < nb_entries; ++i) {
        if (static_cast<DYNAMIC_TAGS>(entries[i].d_tag) ==
            DYNAMIC_TAGS::DT_STRTAB) {
          va_offset = this->binary_->virtual_address_to_offset(entries[i].d_un.d_val);
        }
      }

    } else {
      const Elf64_Dyn* entries = reinterpret_cast<const Elf64_Dyn*>(
          this->stream_->read(offset, size));
      size_t nb_entries = size / sizeof(Elf64_Dyn);
      for (size_t i = 0; i < nb_entries; ++i) {
        if (static_cast<DYNAMIC_TAGS>(entries[i].d_tag) ==
            DYNAMIC_TAGS::DT_STRTAB) {
          va_offset = this->binary_->virtual_address_to_offset(entries[i].d_un.d_val);
        }
      }
    }
  }

  if (va_offset > 0) {
    return va_offset;
  } else {
    throw LIEF::conversion_error("Unable to convert VA to offset from segments");
  }
}

uint64_t Parser::get_dynamic_string_table_from_sections(void) const {
  // Find Dynamic string section
  auto&& it_dynamic_string_section = std::find_if(
      std::begin(this->binary_->sections_),
      std::end(this->binary_->sections_),
      [] (const Section* section)
      {
        return section != nullptr and section->name() == ".dynstr" and section->type() == SECTION_TYPES::SHT_STRTAB;
      });


  uint64_t va_offset = 0;
  if (it_dynamic_string_section != std::end(this->binary_->sections_)) {
    va_offset = (*it_dynamic_string_section)->file_offset();
  }

  if (va_offset > 0) {
    return va_offset;
  } else {
    throw LIEF::conversion_error("Unable to convert VA to offset from sections");
  }

}

uint64_t Parser::get_dynamic_string_table(void) const {
  uint64_t offset = 0;
  try {
    offset = this->get_dynamic_string_table_from_segments();
  } catch (const LIEF::conversion_error&) {
    offset = this->get_dynamic_string_table_from_sections();
  }
  return offset;
}


void Parser::link_symbol_version(void) {
  if (this->binary_->dynamic_symbols_.size() == this->binary_->symbol_version_table_.size()) {
    for (size_t i = 0; i < this->binary_->dynamic_symbols_.size(); ++i) {
      this->binary_->dynamic_symbols_[i]->symbol_version_ = this->binary_->symbol_version_table_[i];
    }
  }
}

}
}
