// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the OpenColorIO Project.

#include <cmath>
#include <cstring>
#include <locale>
#include <sstream>

#include <OpenColorIO/OpenColorIO.h>

#include "YamlEmitter.h"


namespace OCIO_NAMESPACE
{

namespace YamlWrite
{

namespace
{

const int REPLACEMENT_CHARACTER = 0xFFFD;

bool IsOneOf(char c, const char * chars)
{
    return c != '\0' && std::strchr(chars, c) != nullptr;
}

bool IsBlankOrBreak(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Blank, line break or end of the string at position i.
bool IsBlankOrBreakOrEnd(const char * str, size_t size, size_t i)
{
    return i >= size || IsBlankOrBreak(str[i]);
}

bool IsNullString(const char * str, size_t size)
{
    const std::string s(str, size);
    return s.empty() || s == "~" || s == "null" || s == "Null" || s == "NULL";
}

// Characters outside of the YAML printable set, at position i.
bool IsNotPrintable(const char * str, size_t size, size_t i)
{
    const unsigned char c = static_cast<unsigned char>(str[i]);
    if (c <= 0x08 || c == 0x0B || c == 0x0C || (c >= 0x0E && c <= 0x1F) || c == 0x7F)
    {
        return true;
    }

    // C1 control characters except NEL, in UTF-8.
    if (c == 0xC2 && i + 1 < size)
    {
        const unsigned char next = static_cast<unsigned char>(str[i + 1]);
        return (next >= 0x80 && next <= 0x84) || (next >= 0x86 && next <= 0x9F);
    }
    return false;
}

bool IsByteOrderMark(const char * str, size_t size, size_t i)
{
    return i + 2 < size && std::memcmp(str + i, "\xEF\xBB\xBF", 3) == 0;
}

// Whether a string can be written without quotes and read back unchanged, with the same
// rules as yaml-cpp.
bool IsValidPlainScalar(const char * str, size_t size, bool flow)
{
    if (IsNullString(str, size))
    {
        return false;
    }

    // Check the start, e.g. "- a" would be a sequence and "? a" a key.
    const char first = str[0];
    if (IsBlankOrBreak(first))
    {
        return false;
    }
    if (flow)
    {
        if (IsOneOf(first, "?,[]{}#&*!|>'\"%@`"))
        {
            return false;
        }
        if (IsOneOf(first, "-:") && (size == 1 || str[1] == ' ' || str[1] == '\t'))
        {
            return false;
        }
    }
    else
    {
        if (IsOneOf(first, ",[]{}#&*!|>'\"%@`"))
        {
            return false;
        }
        if (IsOneOf(first, "-?:") && IsBlankOrBreakOrEnd(str, size, 1))
        {
            return false;
        }
    }

    // Trailing spaces would be lost.
    if (str[size - 1] == ' ')
    {
        return false;
    }

    for (size_t i = 0; i < size; ++i)
    {
        const char c = str[i];

        // End of the scalar, e.g. "a: b" would be a map.
        if (c == ':' && (IsBlankOrBreakOrEnd(str, size, i + 1) ||
                         (flow && IsOneOf(str[i + 1], ",]}"))))
        {
            return false;
        }
        if (flow && IsOneOf(c, ",?[]{}"))
        {
            return false;
        }

        // Start of a comment.
        if (IsBlankOrBreak(c) && i + 1 < size && str[i + 1] == '#')
        {
            return false;
        }

        if (IsNotPrintable(str, size, i) || IsByteOrderMark(str, size, i) ||
            c == '\n' || c == '\r' || c == '\t' || c == '&')
        {
            return false;
        }
    }

    return true;
}

int Utf8BytesIndicated(char c)
{
    switch (static_cast<unsigned char>(c) >> 4)
    {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
            return 1;
        case 12: case 13:
            return 2;
        case 14:
            return 3;
        case 15:
            return 4;
        default:
            return -1;
    }
}

bool IsTrailingByte(char c)
{
    return (c & 0xC0) == 0x80;
}

// Decode the next UTF-8 code point. Invalid sequences and code points are replaced by
// U+FFFD, like yaml-cpp.
bool GetNextCodePointAndAdvance(int & codePoint, const char *& first, const char * last)
{
    if (first == last)
    {
        return false;
    }

    int nBytes = Utf8BytesIndicated(*first);
    if (nBytes < 1)
    {
        ++first;
        codePoint = REPLACEMENT_CHARACTER;
        return true;
    }

    if (nBytes == 1)
    {
        codePoint = *first++;
        return true;
    }

    codePoint = static_cast<unsigned char>(*first) & ~(0xFF << (7 - nBytes));
    ++first;
    --nBytes;
    for (; nBytes > 0; ++first, --nBytes)
    {
        if (first == last || !IsTrailingByte(*first))
        {
            codePoint = REPLACEMENT_CHARACTER;
            break;
        }
        codePoint <<= 6;
        codePoint |= *first & 0x3F;
    }

    if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF) ||
        (codePoint & 0xFFFE) == 0xFFFE || (codePoint >= 0xFDD0 && codePoint <= 0xFDEF))
    {
        codePoint = REPLACEMENT_CHARACTER;
    }
    return true;
}

} // namespace

void Emitter::setLocal(Setting setting, bool value)
{
    m_settingChanges.push_back({ setting, m_settings[setting] });
    m_settings[setting] = value;
}

void Emitter::restore(const SettingChanges & changes)
{
    // Restored in order like yaml-cpp, so a setting changed twice keeps its first change.
    for (const SettingChange & change : changes)
    {
        m_settings[change.m_setting] = change.m_oldValue;
    }
}

size_t Emitter::curGroupChildCount() const
{
    return m_groups.empty() ? m_docCount : m_groups.back().m_childCount;
}

size_t Emitter::lastIndent() const
{
    return m_groups.size() <= 1 ? 0 : m_curIndent - m_indent;
}

Emitter::NodeType Emitter::nextGroupType(bool isMap) const
{
    // Children of flow collections are always flow.
    const bool flow = curGroupFlow() || m_settings[isMap ? MapFlow : SeqFlow];
    if (isMap)
    {
        return flow ? NodeType::FlowMap : NodeType::BlockMap;
    }
    return flow ? NodeType::FlowSeq : NodeType::BlockSeq;
}

void Emitter::beginGroup(bool isMap)
{
    prepareNode(nextGroupType(isMap));
    startedNode();

    m_curIndent += curGroupIndent();

    // Local settings last until the end of the group.
    Group group;
    group.m_isMap = isMap;
    group.m_flow = curGroupFlow() || m_settings[isMap ? MapFlow : SeqFlow];
    group.m_settingChanges = std::move(m_settingChanges);
    m_settingChanges.clear();

    m_groups.push_back(std::move(group));
}

void Emitter::endGroup(bool isMap)
{
    if (m_groups.empty() || m_groups.back().m_isMap != isMap)
    {
        throw Exception("YAML emitter: unmatched end of collection.");
    }

    Group & group = m_groups.back();
    const bool originalFlow = group.m_flow;

    // Empty collections are always written in flow style, e.g. "[]".
    if (group.m_childCount == 0)
    {
        group.m_flow = true;
    }

    if (group.m_flow)
    {
        if (isMap || !originalFlow || hasBegunNode())
        {
            indentTo(m_curIndent);
        }
        if (!originalFlow || (group.m_childCount == 0 && !hasBegunNode()))
        {
            write(isMap ? '{' : '[');
        }
        write(isMap ? '}' : ']');
    }

    const SettingChanges groupChanges = std::move(group.m_settingChanges);
    m_groups.pop_back();
    m_curIndent -= curGroupIndent();

    restore(groupChanges);
    restore(m_settingChanges);
    m_settingChanges.clear();

    m_hasTag = false;
    m_hasNonContent = false;
}

void Emitter::startedNode()
{
    if (m_groups.empty())
    {
        m_docCount++;
    }
    else
    {
        Group & group = m_groups.back();
        group.m_childCount++;
        if (group.m_childCount % 2 == 0)
        {
            group.m_longKey = false;
        }
    }

    m_hasTag = false;
    m_hasNonContent = false;
}

void Emitter::startedScalar()
{
    startedNode();
    restore(m_settingChanges);
    m_settingChanges.clear();
}

// Write what comes before the next node, e.g. "- " in a block sequence.
void Emitter::prepareNode(NodeType child)
{
    if (m_groups.empty())
    {
        prepareTopNode(child);
        return;
    }

    const Group & group = m_groups.back();
    if (group.m_isMap)
    {
        group.m_flow ? flowMapPrepareNode(child) : blockMapPrepareNode(child);
    }
    else
    {
        group.m_flow ? flowSeqPrepareNode(child) : blockSeqPrepareNode(child);
    }
}

void Emitter::prepareTopNode(NodeType child)
{
    switch (child)
    {
        case NodeType::None:
            break;
        case NodeType::Property:
        case NodeType::Scalar:
        case NodeType::FlowSeq:
        case NodeType::FlowMap:
            spaceOrIndentTo(hasBegunContent(), 0);
            break;
        case NodeType::BlockSeq:
        case NodeType::BlockMap:
            if (hasBegunNode())
            {
                write('\n');
            }
            break;
    }
}

void Emitter::flowSeqPrepareNode(NodeType child)
{
    const size_t indent = lastIndent();
    const size_t childCount = curGroupChildCount();

    if (!hasBegunNode())
    {
        indentTo(indent);
        write(childCount == 0 ? '[' : ',');
    }

    if (child != NodeType::None)
    {
        spaceOrIndentTo(hasBegunContent() || childCount > 0, indent);
    }
}

void Emitter::blockSeqPrepareNode(NodeType child)
{
    if (child == NodeType::None)
    {
        return;
    }

    if (!hasBegunContent())
    {
        if (curGroupChildCount() > 0)
        {
            write('\n');
        }
        indentTo(m_curIndent);
        write('-');
    }

    switch (child)
    {
        case NodeType::None:
            break;
        case NodeType::Property:
        case NodeType::Scalar:
        case NodeType::FlowSeq:
        case NodeType::FlowMap:
            spaceOrIndentTo(hasBegunContent(), m_curIndent + curGroupIndent());
            break;
        case NodeType::BlockSeq:
            write('\n');
            break;
        case NodeType::BlockMap:
            if (hasBegunContent())
            {
                write('\n');
            }
            break;
    }
}

void Emitter::flowMapPrepareNode(NodeType child)
{
    Group & group = m_groups.back();
    const size_t indent = lastIndent();
    const bool isKey = group.m_childCount % 2 == 0;

    if (isKey && m_settings[LongKey])
    {
        group.m_longKey = true;
    }

    if (!hasBegunNode())
    {
        indentTo(indent);
        if (isKey)
        {
            write(group.m_childCount == 0 ? '{' : ',');
            if (group.m_longKey)
            {
                write(" ?");
            }
        }
        else
        {
            write(':');
        }
    }

    if (child != NodeType::None)
    {
        spaceOrIndentTo(hasBegunContent() || group.m_childCount > 0, indent);
    }
}

void Emitter::blockMapPrepareNode(NodeType child)
{
    Group & group = m_groups.back();
    const size_t childCount = group.m_childCount;
    const bool isBlockChild = child == NodeType::BlockSeq || child == NodeType::BlockMap;
    const bool isKey = childCount % 2 == 0;

    // Keys that are not simple scalars use the explicit "? key" form.
    if (isKey && (m_settings[LongKey] || isBlockChild || child == NodeType::Property))
    {
        group.m_longKey = true;
    }

    if (group.m_longKey)
    {
        if (child == NodeType::None)
        {
            return;
        }

        if (!hasBegunContent())
        {
            if (!isKey || childCount > 0)
            {
                write('\n');
            }
            indentTo(m_curIndent);
            write(isKey ? '?' : ':');
        }

        if (isBlockChild && hasBegunContent())
        {
            write('\n');
        }
        if (!isBlockChild || !isKey)
        {
            spaceOrIndentTo(true, m_curIndent + 1);
        }
        return;
    }

    if (isKey)
    {
        if (child == NodeType::None)
        {
            return;
        }

        if (!hasBegunNode() && childCount > 0)
        {
            write('\n');
        }
        if (!isBlockChild)
        {
            spaceOrIndentTo(hasBegunContent(), m_curIndent);
        }
        return;
    }

    if (!hasBegunNode())
    {
        write(':');
    }

    if (isBlockChild)
    {
        write('\n');
    }
    else if (child != NodeType::None)
    {
        spaceOrIndentTo(true, m_curIndent + curGroupIndent());
    }
}

void Emitter::spaceOrIndentTo(bool requireSpace, size_t indent)
{
    if (m_col > 0 && requireSpace)
    {
        write(' ');
    }
    indentTo(indent);
}

Emitter & Emitter::operator<<(Manipulator manipulator)
{
    switch (manipulator)
    {
        case Key:
        case Value:
            break;
        case BeginMap:
            beginGroup(true);
            break;
        case EndMap:
            endGroup(true);
            break;
        case BeginSeq:
            beginGroup(false);
            break;
        case EndSeq:
            endGroup(false);
            break;
        case Flow:
        case Block:
            setLocal(SeqFlow, manipulator == Flow);
            setLocal(MapFlow, manipulator == Flow);
            break;
        case Literal:
            setLocal(LiteralString, true);
            break;
        case Newline:
            prepareNode(NodeType::None);
            write('\n');
            m_hasNonContent = true;
            break;
    }
    return *this;
}

Emitter & Emitter::operator<<(const VerbatimTag & tag)
{
    prepareNode(NodeType::Property);
    write("!<");
    write(tag.m_name.c_str(), tag.m_name.size());
    write('>');
    m_hasTag = true;
    return *this;
}

Emitter & Emitter::operator<<(const std::string & value)
{
    writeString(value.c_str(), value.size());
    return *this;
}

Emitter & Emitter::operator<<(const char * value)
{
    writeString(value, std::strlen(value));
    return *this;
}

Emitter & Emitter::operator<<(bool value)
{
    prepareNode(NodeType::Scalar);
    write(value ? "true" : "false");
    startedScalar();
    return *this;
}

// Letters are written plain, other characters quoted, e.g. "5" and " ".
Emitter & Emitter::operator<<(char value)
{
    prepareNode(NodeType::Scalar);

    if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z'))
    {
        write(value);
    }
    else if ((value >= 0x20 && value <= 0x7E) || IsOneOf(value, "\t\n\b\r\f"))
    {
        writeDoubleQuoted(&value, 1);
    }
    else
    {
        // Control and non-ASCII characters are escaped individually, e.g. "\x1b".
        static const char hexDigits[] = "0123456789abcdef";
        const unsigned char c = static_cast<unsigned char>(value);
        write("\"\\x");
        write(hexDigits[c >> 4]);
        write(hexDigits[c & 0xF]);
        write('"');
    }

    startedScalar();
    return *this;
}

Emitter & Emitter::operator<<(int value)
{
    prepareNode(NodeType::Scalar);
    write(std::to_string(value).c_str());
    startedScalar();
    return *this;
}

Emitter & Emitter::operator<<(unsigned value)
{
    prepareNode(NodeType::Scalar);
    write(std::to_string(value).c_str());
    startedScalar();
    return *this;
}

Emitter & Emitter::operator<<(float value)
{
    writeFloat(value, m_floatPrecision);
    return *this;
}

Emitter & Emitter::operator<<(double value)
{
    writeFloat(value, m_doublePrecision);
    return *this;
}

void Emitter::writeString(const char * str, size_t size)
{
    StringFormat format = StringFormat::DoubleQuoted;
    if (m_settings[LiteralString])
    {
        if (!curGroupFlow())
        {
            format = StringFormat::Literal;
        }
    }
    else if (IsValidPlainScalar(str, size, curGroupFlow()))
    {
        format = StringFormat::Plain;
    }

    if (format == StringFormat::Literal || size > 1024)
    {
        setLocal(LongKey, true);
    }

    prepareNode(NodeType::Scalar);

    switch (format)
    {
        case StringFormat::Plain:
            write(str, size);
            break;
        case StringFormat::DoubleQuoted:
            writeDoubleQuoted(str, size);
            break;
        case StringFormat::Literal:
            writeLiteral(str, size, m_curIndent + m_indent);
            break;
    }

    startedScalar();
}

// Shortest of fixed or scientific notation with the given number of significant digits,
// like printf with %g.
template<typename T>
void Emitter::writeFloat(T value, int precision)
{
    prepareNode(NodeType::Scalar);

    if (std::isnan(value))
    {
        write(".nan");
    }
    else if (std::isinf(value))
    {
        write(std::signbit(value) ? "-.inf" : ".inf");
    }
    else
    {
        std::ostringstream os;
        os.imbue(std::locale::classic());
        os.precision(precision);
        os << value;
        const std::string str = os.str();
        write(str.c_str(), str.size());
    }

    startedScalar();
}

void Emitter::write(const char * str, size_t size)
{
    m_out.append(str, size);

    // Track the column for indentation.
    for (size_t i = 0; i < size; ++i)
    {
        m_col = (str[i] == '\n') ? 0 : m_col + 1;
    }
}

void Emitter::write(const char * str)
{
    write(str, std::strlen(str));
}

void Emitter::write(char c)
{
    m_out.push_back(c);
    m_col = (c == '\n') ? 0 : m_col + 1;
}

void Emitter::indentTo(size_t column)
{
    while (m_col < column)
    {
        write(' ');
    }
}

void Emitter::writeCodePoint(int codePoint)
{
    if (codePoint < 0 || codePoint > 0x10FFFF)
    {
        codePoint = REPLACEMENT_CHARACTER;
    }

    if (codePoint <= 0x7F)
    {
        write(static_cast<char>(codePoint));
    }
    else if (codePoint <= 0x7FF)
    {
        write(static_cast<char>(0xC0 | (codePoint >> 6)));
        write(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else if (codePoint <= 0xFFFF)
    {
        write(static_cast<char>(0xE0 | (codePoint >> 12)));
        write(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        write(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else
    {
        write(static_cast<char>(0xF0 | (codePoint >> 18)));
        write(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        write(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        write(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

void Emitter::writeDoubleQuoted(const char * str, size_t size)
{
    static const char hexDigits[] = "0123456789abcdef";

    write('"');
    int codePoint;
    for (const char * i = str; GetNextCodePointAndAdvance(codePoint, i, str + size);)
    {
        switch (codePoint)
        {
            case '"':  write("\\\""); break;
            case '\\': write("\\\\"); break;
            case '\n': write("\\n"); break;
            case '\t': write("\\t"); break;
            case '\r': write("\\r"); break;
            case '\b': write("\\b"); break;
            case '\f': write("\\f"); break;
            default:
                // Escape control characters, non-breaking space and byte order marks,
                // e.g. \x1b, \xa0 and ﻿.
                if (codePoint < 0x20 || (codePoint >= 0x80 && codePoint <= 0xA0) ||
                    codePoint == 0xFEFF)
                {
                    write('\\');
                    int digits = 8;
                    if (codePoint < 0xFF)
                    {
                        write('x');
                        digits = 2;
                    }
                    else if (codePoint < 0xFFFF)
                    {
                        write('u');
                        digits = 4;
                    }
                    else
                    {
                        write('U');
                    }
                    for (; digits > 0; --digits)
                    {
                        write(hexDigits[(codePoint >> (4 * (digits - 1))) & 0xF]);
                    }
                }
                else
                {
                    writeCodePoint(codePoint);
                }
                break;
        }
    }
    write('"');
}

void Emitter::writeLiteral(const char * str, size_t size, size_t indent)
{
    write("|\n");
    int codePoint;
    for (const char * i = str; GetNextCodePointAndAdvance(codePoint, i, str + size);)
    {
        if (codePoint == '\n')
        {
            write('\n');
        }
        else
        {
            indentTo(indent);
            writeCodePoint(codePoint);
        }
    }
}

} // namespace YamlWrite

} // namespace OCIO_NAMESPACE
