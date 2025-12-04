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
  stream_(nullptr)
{
}

LibertyBinaryReader::~LibertyBinaryReader()
{
  // stream_ is not owned if passed via read(std::istream*)
  // But if opened via read(filename), we might need to close it?
  // Actually, read(filename) uses local ifstream, so no need to close.
  // stmts_ are deleted by readGroup/readAttr logic if not saved
  for (LibertyStmt *stmt : stmts_)
    delete stmt;
}



bool
LibertyBinaryReader::read(std::istream *stream)
{
  stream_ = stream;
  
  char magic[9];
  stream_->read(magic, 8);
  if (!stream_->good()) return false;
  magic[8] = '\0';
  if (strcmp(magic, LIBERTY_BINARY_MAGIC) != 0) return false;

  std::uint32_t version;
  stream_->read(reinterpret_cast<char*>(&version), sizeof(version));
  if (!stream_->good()) return false;
  // Check version compatibility if needed

  while (true) {
    std::uint8_t tag_val;
    stream_->read(reinterpret_cast<char*>(&tag_val), sizeof(tag_val));
    if (!stream_->good()) break;
    LibertyBinaryTag tag = static_cast<LibertyBinaryTag>(tag_val);
    
    if (tag == LibertyBinaryTag::EOF_TAG) break;
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

  LibertyGroup *group = new LibertyGroup(type.c_str(), params, 0); // Line 0 for binary
  visitor_->begin(group);

  while (true) {
    std::uint8_t tag_val;
    stream_->read(reinterpret_cast<char*>(&tag_val), sizeof(tag_val));
    if (!stream_->good()) break; // Error
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
  LibertyAttr *attr = new LibertySimpleAttr(name.c_str(), val, 0);
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
  for (std::uint32_t i = 0; i < count; i++) {
    values->push_back(readValue());
  }
  LibertyAttr *attr = new LibertyComplexAttr(name.c_str(), values, 0);
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
  LibertyVariable *var = new LibertyVariable(name.c_str(), val, 0);
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
  std::uint8_t type;
  stream_->read(reinterpret_cast<char*>(&type), sizeof(type));
  if (!stream_->good()) return "";
  // Check type?
  
  std::uint32_t len;
  stream_->read(reinterpret_cast<char*>(&len), sizeof(len));
  if (!stream_->good()) return "";
  
  if (len == 0) return "";
  
  std::string str(len, '\0');
  stream_->read(&str[0], len);
  if (!stream_->good()) return "";
  return str;
}

float
LibertyBinaryReader::readFloat()
{
  std::uint8_t type;
  stream_->read(reinterpret_cast<char*>(&type), sizeof(type));
  float val;
  stream_->read(reinterpret_cast<char*>(&val), sizeof(val));
  return val;
}

int
LibertyBinaryReader::readInt()
{
  std::uint8_t type;
  stream_->read(reinterpret_cast<char*>(&type), sizeof(type));
  int val;
  stream_->read(reinterpret_cast<char*>(&val), sizeof(val));
  return val;
}

std::uint32_t
LibertyBinaryReader::readUInt32()
{
  std::uint32_t val;
  stream_->read(reinterpret_cast<char*>(&val), sizeof(val));
  if (!stream_->good()) return 0;
  return val;
}

bool
LibertyBinaryReader::readBool()
{
  std::uint8_t type;
  stream_->read(reinterpret_cast<char*>(&type), sizeof(type));
  std::uint8_t val;
  stream_->read(reinterpret_cast<char*>(&val), sizeof(val));
  return val != 0;
}

LibertyAttrValue *
LibertyBinaryReader::readValue()
{
  std::uint8_t type;
  stream_->read(reinterpret_cast<char*>(&type), sizeof(type));
  if (!stream_->good()) return nullptr;
  
  stream_->putback(static_cast<char>(type));
  
  LibertyBinaryValueType val_type = static_cast<LibertyBinaryValueType>(type);
  if (val_type == LibertyBinaryValueType::STRING) {
    return new LibertyStringAttrValue(readString().c_str());
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
  
  return nullptr;
}

} // namespace
