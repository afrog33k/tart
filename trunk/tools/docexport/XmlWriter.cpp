/* ================================================================ *
    TART - A Sweet Programming Language.
 * ================================================================ */

#include "XmlWriter.h"
#include "tart/Common/Diagnostics.h"

namespace tart {

using namespace llvm;

namespace {
  const char SPACES[] = "                                ";
}

XmlWriter & XmlWriter::writeXmlPrologue() {
  beginProcessingInstruction("xml");
  appendAttribute("encoding", "UTF-8");
  endProcessingInstruction();
  return *this;
}

XmlWriter & XmlWriter::beginElement(StringRef elName) {
  closeCurrentElement();
  writeIndent();
  strm_ << '<' << elName;
  state_ = ELEMENT;
  simpleElement_ = true;
  ++indentLevel_;
  return *this;
}

XmlWriter & XmlWriter::endElement(StringRef elName) {
  --indentLevel_;
  switch (state_) {
    case PI:
      DFAIL("Cannot close element because we are in a processing instruction");

    case ELEMENT:
      state_ = CHARACTERS;
      simpleElement_ = false;
      strm_ << (formatPretty_ ? "/>\n" : "/>");
      break;

    case CDATA:
      closeCurrentElement();
      strm_ << "</" << elName << (formatPretty_ ? ">\n" : ">");
      simpleElement_ = false;
      break;

    case CHARACTERS:
      if (!simpleElement_) {
        writeIndent();
      }
      simpleElement_ = false;
      strm_ << "</" << elName << (formatPretty_ ? ">\n" : ">");
      break;
  }
  return *this;
}

XmlWriter & XmlWriter::beginProcessingInstruction(StringRef psName) {
  closeCurrentElement();
  strm_ << "<?" << psName;
  simpleElement_ = false;
  state_ = PI;
  return *this;
}

XmlWriter & XmlWriter::endProcessingInstruction() {
  DASSERT_MSG(state_ == PI, "Not in a processing instruction");
  strm_ << "?>";
  state_ = CHARACTERS;
  return *this;
}

XmlWriter & XmlWriter::appendAttribute(StringRef name, StringRef value) {
  DASSERT(state_ == ELEMENT || state_ == PI);
  strm_ << ' ' << name << "=\"";
  writeEscapedString(value);
  strm_ << "\"";
  return *this;
}

XmlWriter & XmlWriter::writeCharacterData(StringRef data) {
  closeCurrentElement(false);
  writeEscapedString(data);
  return *this;
}

XmlWriter & XmlWriter::writeCDATA(StringRef data) {
  if (state_ != CDATA) {
    closeCurrentElement();
    strm_ << "<![CDATA[";
  }
  strm_.write(data.data(), data.size());
  return *this;
}

XmlWriter & XmlWriter::writeComment(StringRef data) {
  closeCurrentElement();
  writeIndent();
  strm_ << "<!-- ";
  char lastCh = 0;
  for (StringRef::const_iterator it = data.begin(), itEnd = data.end(); it != itEnd; ++it) {
    char ch = *it;
    if (ch == '-' && lastCh == '-') {
      continue;
    }
    strm_ << ch;
    lastCh = ch;
  }
  strm_ << " -->";
  return *this;
}

void XmlWriter::closeCurrentElement(bool newline) {
  switch (state_) {
    case CHARACTERS: break;
    case ELEMENT:
      strm_ << (newline && formatPretty_ ? ">\n" : ">");
      break;
    case PI:
      strm_ << (newline && formatPretty_ ? "?>\n" : "?>");
      break;
    case CDATA:
      strm_ << (newline && formatPretty_ ? "]]>\n" : "]]>");
      break;
  }
  state_ = CHARACTERS;
}

void XmlWriter::writeIndent() {
  if (formatPretty_) {
    int32_t indent = indentLevel_ * indentSpaces_;
    while (indent > 0) {
      // Can write up to 32 spaces at a time.
      int32_t spaces = std::min(indent, 32);
      strm_.write(SPACES, spaces);
      indent -= spaces;
    }
  }
}

void XmlWriter::writeEscapedString(StringRef text) {
  for (StringRef::const_iterator it = text.begin(), itEnd = text.end(); it != itEnd; ++it) {
    char ch = *it;
    switch (ch) {
      case '&': strm_ << "&amp;"; break;
      case '<': strm_ << "&lt;"; break;
      case '>': strm_ << "&gt;"; break;
      case '"': strm_ << "&quot;"; break;
      case '\'': strm_ << "&apos;"; break;
      default:
        strm_ << ch;
        break;
    }
  }
}

}
