/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fields/array_field.h"
#include "util.h"

const std::string ArrayField::kFieldType = "ArrayField";

ArrayField::ArrayField(std::string name, int element_size, std::string size_modifier, ParseLocation loc)
    : PacketField(name, loc), element_size_(element_size), size_modifier_(size_modifier) {
  // Make sure the element_size is a multiple of 8.
  if (element_size_ > 64 || element_size_ < 0)
    ERROR(this) << __func__ << ": Not implemented for element size = " << element_size_;
  if (element_size % 8 != 0) {
    ERROR(this) << "Can only have arrays with elements that are byte aligned (" << element_size << ")";
  }
}

ArrayField::ArrayField(std::string name, int element_size, int fixed_size, ParseLocation loc)
    : PacketField(name, loc), element_size_(element_size), fixed_size_(fixed_size) {
  if (element_size_ > 64 || element_size_ < 0)
    ERROR(this) << __func__ << ": Not implemented for element size = " << element_size_;
  // Make sure the element_size is a multiple of 8.
  if (element_size % 8 != 0) {
    ERROR(this) << "Can only have arrays with elements that are byte aligned (" << element_size << ")";
  }
}

ArrayField::ArrayField(std::string name, TypeDef* type_def, std::string size_modifier, ParseLocation loc)
    : PacketField(name, loc), element_size_(type_def->size_), type_def_(type_def), size_modifier_(size_modifier) {
  // If the element type is not variable sized, make sure that it is byte aligned.
  if (type_def_->size_ != -1 && type_def_->size_ % 8 != 0) {
    ERROR(this) << "Can only have arrays with elements that are byte aligned (" << type_def_->size_ << ")";
  }
}

ArrayField::ArrayField(std::string name, TypeDef* type_def, int fixed_size, ParseLocation loc)
    : PacketField(name, loc), element_size_(type_def->size_), type_def_(type_def), fixed_size_(fixed_size) {
  // If the element type is not variable sized, make sure that it is byte aligned.
  if (type_def_->size_ != -1 && type_def_->size_ % 8 != 0) {
    ERROR(this) << "Can only have arrays with elements that are byte aligned (" << type_def_->size_ << ")";
  }
}

const std::string& ArrayField::GetFieldType() const {
  return ArrayField::kFieldType;
}

Size ArrayField::GetSize() const {
  if (IsFixedSize() && element_size_ != -1) {
    return Size(fixed_size_ * element_size_);
  }

  // If there is no size field, then it is of unknown size.
  if (size_field_ == nullptr) {
    return Size();
  }

  // size_field_ is of type SIZE
  if (size_field_->GetFieldType() == SizeField::kFieldType) {
    std::string ret = "Get" + util::UnderscoreToCamelCase(size_field_->GetName()) + "()";
    if (!size_modifier_.empty()) ret += size_modifier_;
    return ret;
  }

  // size_field_ is of type COUNT and it is a scalar array
  if (!IsEnumArray() && !IsCustomFieldArray()) {
    return "(Get" + util::UnderscoreToCamelCase(size_field_->GetName()) + "() * " + std::to_string(element_size_ / 8) +
           ")";
  }

  if (IsCustomFieldArray()) {
    if (type_def_->size_ != -1) {
      return "(Get" + util::UnderscoreToCamelCase(size_field_->GetName()) + "() * " +
             std::to_string(type_def_->size_ / 8) + ")";
    } else {
      return Size();
    }
  }

  // size_field_ is of type COUNT and it is an enum array
  return "(Get" + util::UnderscoreToCamelCase(size_field_->GetName()) + "() * " + std::to_string(type_def_->size_ / 8) +
         ")";
}

Size ArrayField::GetBuilderSize() const {
  if (element_size_ != -1) {
    std::string ret = "(" + GetName() + "_.size() * " + std::to_string(element_size_) + ")";
    return ret;
  } else {
    std::string ret = "[this](){ size_t length = 0; for (const auto& elem : " + GetName() +
                      "_) { length += elem.size() * 8; } return length; }()";
    return ret;
  }
}

std::string ArrayField::GetDataType() const {
  if (type_def_ != nullptr) {
    return "std::vector<" + type_def_->name_ + ">";
  }
  return "std::vector<" + util::GetTypeForSize(element_size_) + ">";
}

void ArrayField::GenGetter(std::ostream& s, Size start_offset, Size end_offset) const {
  if (start_offset.empty()) {
    ERROR(this) << "Can not have an array with an ambiguous start offset.";
  }

  if (start_offset.bits() % 8 != 0) {
    ERROR(this) << "Can not have an array that isn't byte aligned.";
  }

  if (GetSize().empty() && end_offset.empty()) {
    ERROR(this) << "Ambiguous end offset for array with no defined size.";
  }

  s << GetDataType();
  s << " Get" << util::UnderscoreToCamelCase(GetName()) << "() {";
  s << "ASSERT(was_validated_);";

  s << "auto it = begin() + " << start_offset.bytes() << " + " << start_offset.dynamic_string() << ";";

  if (!GetSize().empty()) {
    auto size = GetSize();
    s << "auto array_end = it + " << size.bytes() << " /* bytes */ + " << size.dynamic_string() << ";";
  } else {
    s << "auto array_end = end() - " << end_offset.bytes() << " /* bytes */ - " << end_offset.dynamic_string() << ";";
  }

  // Add the element size so that we will extract as many elements as we can.
  s << GetDataType() << " ret;";
  if (element_size_ != -1) {
    std::string type = (type_def_ != nullptr) ? type_def_->name_ : util::GetTypeForSize(element_size_);
    s << "while (it + sizeof(" << type << ") <= array_end) {";
    s << "ret.push_back(it.extract<" << type << ">());";
    s << "}";
  } else {
    s << "while (it < array_end) {";
    s << "it = " << type_def_->name_ << "::Parse(ret, it);";
    s << "}";
  }

  s << "return ret;";
  s << "}\n";
}

bool ArrayField::GenBuilderParameter(std::ostream& s) const {
  if (type_def_ != nullptr) {
    s << "const std::vector<" << type_def_->GetTypeName() << ">& " << GetName();
  } else {
    s << "const std::vector<" << util::GetTypeForSize(element_size_) << ">& " << GetName();
  }
  return true;
}

bool ArrayField::GenBuilderMember(std::ostream& s) const {
  if (type_def_ != nullptr) {
    s << "std::vector<" << type_def_->GetTypeName() << "> " << GetName();
  } else {
    s << "std::vector<" << util::GetTypeForSize(element_size_) << "> " << GetName();
  }
  return true;
}

bool ArrayField::HasParameterValidator() const {
  if (fixed_size_ == -1) {
    // Does not have parameter validator yet.
    // TODO: See comment in GenParameterValidator
    return false;
  }
  return true;
}

void ArrayField::GenParameterValidator(std::ostream& s) const {
  if (fixed_size_ == -1) {
    // No Parameter validator if its dynamically size.
    // TODO: Maybe add a validator to ensure that the size isn't larger than what the size field can hold.
    return;
  }

  s << "ASSERT(" << GetName() << "_.size() == " << fixed_size_ << ");";
}

void ArrayField::GenInserter(std::ostream& s) const {
  s << "for (const auto& val : " << GetName() << "_) {";
  if (IsEnumArray()) {
    s << "insert(static_cast<" << util::GetTypeForSize(type_def_->size_) << ">(val), i, " << type_def_->size_ << ");";
  } else if (IsCustomFieldArray()) {
    if (type_def_->size_ == -1) {
      s << "val.Serialize(i);";
    } else {
      s << "insert(val, i);";
    }
  } else {
    s << "insert(val, i, " << element_size_ << ");";
  }
  s << "}\n";
}

void ArrayField::GenValidator(std::ostream&) const {
  // NOTE: We could check if the element size divides cleanly into the array size, but we decided to forgo that
  // in favor of just returning as many elements as possible in a best effort style.
  //
  // Other than that there is nothing that arrays need to be validated on other than length so nothing needs to
  // be done here.
}

bool ArrayField::IsEnumArray() const {
  return type_def_ != nullptr && type_def_->GetDefinitionType() == TypeDef::Type::ENUM;
}

bool ArrayField::IsCustomFieldArray() const {
  return type_def_ != nullptr && type_def_->GetDefinitionType() == TypeDef::Type::CUSTOM;
}

bool ArrayField::IsFixedSize() const {
  return fixed_size_ != -1;
}

void ArrayField::SetSizeField(const SizeField* size_field) {
  if (size_field->GetFieldType() == CountField::kFieldType && !size_modifier_.empty()) {
    ERROR(this, size_field) << "Can not use count field to describe array with a size modifier."
                            << " Use size instead";
  }

  if (IsFixedSize()) {
    ERROR(this, size_field) << "Can not use size field with a fixed size array.";
  }

  size_field_ = size_field;
}

const std::string& ArrayField::GetSizeModifier() const {
  return size_modifier_;
}
