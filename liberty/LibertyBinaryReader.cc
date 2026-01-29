#include "LibertyBinaryReader.hh"
#include "LibertyBinaryCommon.hh"
#include "LibertyParser.hh"
#include "Report.hh"

#include <cstring>
#include <fstream>

namespace sta {

LibertyBinaryReader::LibertyBinaryReader(LibertyGroupVisitor *visitor,
                                         Report *report) :
  visitor_(visitor),
  report_(report),
  cursor_(nullptr, 0)
{
}

LibertyBinaryReader::~LibertyBinaryReader()
{
  for (LibertyStmt *stmt : stmts_)
    delete stmt;
}

bool
LibertyBinaryReader::read(std::istream *stream)
{
  stream->seekg(0, std::ios::end);
  size_t size = stream->tellg();
  stream->seekg(0, std::ios::beg);
  
  std::vector<char> buffer(size);
  stream->read(buffer.data(), size);
  
  // 2. Use the cursor
  cursor_ = BinaryCursor(buffer.data(), size);

  char magic[9];
  cursor_.readBytes(magic, 8);
  magic[8] = '\0';
  if (strcmp(magic, LIBERTY_BINARY_MAGIC) != 0) return false;

  std::uint32_t version = cursor_.readU32();
  std::uint64_t string_table_offset = cursor_.readU64();

  const char* body_start = cursor_.current();
  cursor_.seek(string_table_offset);
  readStringTable();
  cursor_.setPtr(body_start);


  while (true) {
    std::uint8_t tag_val = cursor_.readU8();
    LibertyBinaryTag tag = static_cast<LibertyBinaryTag>(tag_val);
    
    if (tag == LibertyBinaryTag::EOF_TAG) return true;
    else if (tag == LibertyBinaryTag::GROUP_BEGIN) readGroup(nullptr);
    else if (tag == LibertyBinaryTag::ATTR_SIMPLE) readSimpleAttr(nullptr);
    else if (tag == LibertyBinaryTag::ATTR_COMPLEX) readComplexAttr(nullptr);
    else if (tag == LibertyBinaryTag::VARIABLE) readVariable(nullptr);
    else {
      // Error or unknown tag
      break;
    }
  }
  
  return true;
}

void
LibertyBinaryReader::readGroup(LibertyGroup *parent)
{
  std::string type = readString();
  std::uint32_t param_count = readUInt32();

  LibertyAttrValueSeq *params = new LibertyAttrValueSeq;
  for (std::uint32_t i = 0; i < param_count; i++) {
    params->push_back(readValue());
  }

  LibertyGroup *group = new LibertyGroup(std::move(type), params, 0); // Line 0 for binary
  visitor_->begin(group);

  while (true) {
    std::uint8_t tag_val = cursor_.readU8();
    LibertyBinaryTag tag = static_cast<LibertyBinaryTag>(tag_val);
    
    if (tag == LibertyBinaryTag::GROUP_END) break;
    else if (tag == LibertyBinaryTag::GROUP_BEGIN) readGroup(group);
    else if (tag == LibertyBinaryTag::ATTR_SIMPLE) readSimpleAttr(group);
    else if (tag == LibertyBinaryTag::ATTR_COMPLEX) readComplexAttr(group);
    else if (tag == LibertyBinaryTag::VARIABLE) readVariable(group);
    else if (tag == LibertyBinaryTag::EOF_TAG) break; // Should not happen inside group
  }

  visitor_->end(group);

  if (parent && visitor_->save(group)) {
    parent->addSubgroup(group);
  }
  else {
    delete group;
  }
}

void
LibertyBinaryReader::readSimpleAttr(LibertyGroup *parent)
{
  std::string name = readString();
  readUInt32(); // Consume count (should be 1)
  
  // Simple attribute must have exactly 1 value
  LibertyAttrValue *val = readValue();
  LibertyAttr *attr = new LibertySimpleAttr(std::move(name), val, 0);
  visitor_->visitAttr(attr);
  if (parent && visitor_->save(attr)) {
    parent->addAttribute(attr);
  }
  else {
    delete attr;
  }
}

void
LibertyBinaryReader::readComplexAttr(LibertyGroup *parent)
{
  std::string name = readString();
  std::uint32_t count = readUInt32();
  
  LibertyAttrValueSeq *values = new LibertyAttrValueSeq;
  values->reserve(count);
  for (std::uint32_t i = 0; i < count; i++) {
    values->push_back(readValue());
  }
  LibertyAttr *attr = new LibertyComplexAttr(std::move(name), values, 0);
  visitor_->visitAttr(attr);
  if (parent && visitor_->save(attr)) {
    parent->addAttribute(attr);
  }
  else {
    delete attr;
  }
}

void
LibertyBinaryReader::readVariable(LibertyGroup *parent)
{
  std::string name = readString();
  float val = readFloat();
  LibertyVariable *var = new LibertyVariable(std::move(name), val, 0);
  visitor_->visitVariable(var);
  if (visitor_->save(var)) {
    if (parent) parent->addVariable(var);
  }
  else {
    delete var;
  }
}

std::string
LibertyBinaryReader::readString()
{
  std::uint8_t type = cursor_.readU8();
  
  if (static_cast<LibertyBinaryValueType>(type) != LibertyBinaryValueType::STRING) {
    // Should not happen if format is correct
    return "";
  }
  
  std::uint32_t index = cursor_.readU32();
  
  if (index >= string_table_.size()) return "";
  return string_table_[index];
}

void
LibertyBinaryReader::readStringTable()
{
  std::uint32_t size = cursor_.readU32();
  
  string_table_.resize(size);
  std::cout << "Reading string table of size " << size << std::endl;
  for (std::uint32_t i = 0; i < size; i++) {
    std::uint32_t len = cursor_.readU32();
    
    std::string str(len, '\0');
    cursor_.readBytes(&str[0], len);
    
    std::uint32_t index; // The writer writes the value (offset/index) but we just need to fill our vector in order?
    // Wait, the writer writes:
    // out_stream->write(reinterpret_cast<const char*>(&string_length), sizeof(string_length));
    // out_stream->write(entry.first.c_str(), entry.first.size());
    // out_stream->write(reinterpret_cast<const char*>(&entry.second), sizeof(entry.second));
    // entry.second is the offset/index.
    // Since we iterate the map, the order is not guaranteed to be 0, 1, 2... 
    // BUT the writer assigns offsets sequentially: string_table_[str] = offset; offset = string_table_.size();
    // So the indices ARE 0, 1, 2... but the iteration order of unordered_map is random.
    // So we MUST read the index and place it at that index.
    
    index = cursor_.readU32();
    
    if (index >= string_table_.size()) {
      // This shouldn't happen if size is correct and indices are dense
      // But if unordered_map iteration is weird, maybe?
      // Actually indices are 0 to size-1.
      // So we should be safe if we resize to size.
    }
    if (index < string_table_.size()) {
      string_table_[index] = str;
    }
  }
}

float
LibertyBinaryReader::readFloat()
{
  std::uint8_t type = cursor_.readU8();
  float val = cursor_.readFloat();
  return val;
}

int
LibertyBinaryReader::readInt()
{
  std::uint8_t type = cursor_.readU8();
  int val = cursor_.readI32();
  return val;
}

std::uint32_t
LibertyBinaryReader::readUInt32()
{
  std::uint32_t val = cursor_.readU32();
  return val;
}

bool
LibertyBinaryReader::readBool()
{
  std::uint8_t type = cursor_.readU8();
  std::uint8_t val = cursor_.readU8();
  return val != 0;
}

LibertyAttrValue *
LibertyBinaryReader::readValue()
{
  std::uint8_t type = cursor_.peekU8();
    
  LibertyBinaryValueType val_type = static_cast<LibertyBinaryValueType>(type);
  if (val_type == LibertyBinaryValueType::STRING) {
    std::string str = readString();
    return new LibertyStringAttrValue(std::move(str));
  }
  else if (val_type == LibertyBinaryValueType::FLOAT) {
    return new LibertyFloatAttrValue(readFloat());
  }
  else if (val_type == LibertyBinaryValueType::INT) {
    return new LibertyFloatAttrValue((float)readInt());
  }
  else if (val_type == LibertyBinaryValueType::BOOLEAN) {
    bool b = readBool();
    return new LibertyStringAttrValue(b ? "true" : "false");
  }
  else if (val_type == LibertyBinaryValueType::FLOAT_SEQ) {
    cursor_.readU8();
    std::uint32_t count = readUInt32();
    LibertyFloatSeqAttrValue *val = new LibertyFloatSeqAttrValue(const_cast<char*>(cursor_.current()), count);
    cursor_.setPtr(cursor_.current() + (count * sizeof(float)));
    return val;
  }
  
  return nullptr;
}

} // namespace
