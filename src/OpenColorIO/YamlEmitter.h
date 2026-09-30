// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the OpenColorIO Project.

#ifndef INCLUDED_OCIO_YAMLEMITTER_H
#define INCLUDED_OCIO_YAMLEMITTER_H

#include <string>
#include <vector>

#include <OpenColorIO/OpenColorIO.h>

namespace OCIO_NAMESPACE
{

// Streaming YAML writer. Mirrors the subset of the yaml-cpp Emitter API used for writing
// configs, producing the same output: layout, indentation, blank lines and scalar quoting.
namespace YamlWrite
{

enum Manipulator
{
    // Keys and values are deduced from their position in a map, these are only for readability.
    Key,
    Value,
    BeginMap,
    EndMap,
    BeginSeq,
    EndSeq,
    // Style of the next collection and its children.
    Flow,
    Block,
    // Write the next string as a literal block scalar.
    Literal,
    // Write an empty line.
    Newline
};

// Verbatim tag of the next node, written as !<name>.
struct VerbatimTag
{
    explicit VerbatimTag(const char * name) : m_name(name) {}
    std::string m_name;
};

class Emitter
{
public:
    Emitter() = default;

    // Number of significant digits for floating point values.
    void SetFloatPrecision(int precision) { m_floatPrecision = precision; }
    void SetDoublePrecision(int precision) { m_doublePrecision = precision; }

    const std::string & str() const { return m_out; }

    Emitter & operator<<(Manipulator manipulator);
    Emitter & operator<<(const VerbatimTag & tag);

    Emitter & operator<<(const std::string & value);
    Emitter & operator<<(const char * value);
    Emitter & operator<<(bool value);
    Emitter & operator<<(char value);
    Emitter & operator<<(int value);
    Emitter & operator<<(unsigned value);
    Emitter & operator<<(float value);
    Emitter & operator<<(double value);

    template<typename T>
    Emitter & operator<<(const std::vector<T> & values)
    {
        *this << BeginSeq;
        for (const T & value : values)
        {
            *this << value;
        }
        return *this << EndSeq;
    }

private:
    enum class NodeType { None, Property, Scalar, FlowSeq, BlockSeq, FlowMap, BlockMap };
    enum class StringFormat { Plain, DoubleQuoted, Literal };

    // Formatting settings, changed until the next scalar or the end of the next collection.
    enum Setting { SeqFlow, MapFlow, LiteralString, LongKey, NumSettings };
    struct SettingChange
    {
        Setting m_setting;
        bool m_oldValue;
    };
    using SettingChanges = std::vector<SettingChange>;

    struct Group
    {
        bool m_isMap = false;
        bool m_flow = false;
        size_t m_childCount = 0;
        // The current key is written in the explicit "? key" form.
        bool m_longKey = false;
        SettingChanges m_settingChanges;
    };

    void setLocal(Setting setting, bool value);
    void restore(const SettingChanges & changes);

    bool curGroupFlow() const { return !m_groups.empty() && m_groups.back().m_flow; }
    size_t curGroupIndent() const { return m_groups.empty() ? 0 : m_indent; }
    size_t curGroupChildCount() const;
    bool curGroupLongKey() const { return !m_groups.empty() && m_groups.back().m_longKey; }
    size_t lastIndent() const;
    bool hasBegunNode() const { return m_hasTag || m_hasNonContent; }
    bool hasBegunContent() const { return m_hasTag; }
    NodeType nextGroupType(bool isMap) const;

    void beginGroup(bool isMap);
    void endGroup(bool isMap);
    void startedNode();
    void startedScalar();

    void prepareNode(NodeType child);
    void prepareTopNode(NodeType child);
    void flowSeqPrepareNode(NodeType child);
    void blockSeqPrepareNode(NodeType child);
    void flowMapPrepareNode(NodeType child);
    void blockMapPrepareNode(NodeType child);
    void spaceOrIndentTo(bool requireSpace, size_t indent);

    void writeString(const char * str, size_t size);
    template<typename T> void writeFloat(T value, int precision);

    void write(const char * str, size_t size);
    void write(const char * str);
    void write(char c);
    void indentTo(size_t column);
    void writeCodePoint(int codePoint);
    void writeDoubleQuoted(const char * str, size_t size);
    void writeLiteral(const char * str, size_t size, size_t indent);

    std::string m_out;
    size_t m_col = 0;

    const size_t m_indent = 2;
    int m_floatPrecision = 9;
    int m_doublePrecision = 17;

    bool m_settings[NumSettings] = { false, false, false, false };
    SettingChanges m_settingChanges;

    std::vector<Group> m_groups;
    size_t m_curIndent = 0;
    size_t m_docCount = 0;
    bool m_hasTag = false;
    bool m_hasNonContent = false;
};

} // namespace YamlWrite

} // namespace OCIO_NAMESPACE

#endif // INCLUDED_OCIO_YAMLEMITTER_H
