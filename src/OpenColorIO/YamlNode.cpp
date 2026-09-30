// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the OpenColorIO Project.

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

#include <ryml.hpp>

#include <OpenColorIO/OpenColorIO.h>

#include "YamlNode.h"
#include "utils/NumberUtils.h"
#include "utils/StringUtils.h"


namespace OCIO_NAMESPACE
{

namespace YamlRead
{

namespace
{

[[noreturn]] void ErrorBasic(ryml::csubstr msg, ryml::ErrorDataBasic const &, void *)
{
    throw Exception(std::string(msg.str, msg.len).c_str());
}

[[noreturn]] void ErrorParse(ryml::csubstr msg, ryml::ErrorDataParse const & data, void *)
{
    std::ostringstream os;
    if (data.ymlloc.line != ryml::npos)
    {
        os << "error at line " << (data.ymlloc.line + 1) << ": ";
    }
    os << std::string(msg.str, msg.len);
    throw Exception(os.str().c_str());
}

[[noreturn]] void ErrorVisit(ryml::csubstr msg, ryml::ErrorDataVisit const &, void *)
{
    throw Exception(std::string(msg.str, msg.len).c_str());
}

ryml::Callbacks GetCallbacks()
{
    ryml::Callbacks callbacks;
    callbacks.set_error_basic(ErrorBasic);
    callbacks.set_error_parse(ErrorParse);
    callbacks.set_error_visit(ErrorVisit);
    return callbacks;
}

} // namespace

// Parsed tree. The parser is kept for looking up source locations.
class Document
{
public:
    Document()
        : m_handler(GetCallbacks())
        , m_parser(&m_handler)
        , m_tree(GetCallbacks())
    {
    }

    Document(const Document &) = delete;
    Document & operator=(const Document &) = delete;

    // Line of a position in the source. Lines are only needed for error messages, so they are
    // computed on demand rather than letting the parser track locations.
    int line(const char * str) const
    {
        const ryml::csubstr source = m_parser.source();
        if (!str || str < source.begin() || str > source.end())
        {
            return -1;
        }
        return int(std::count(source.begin(), str, '\n'));
    }

    // Line of a node, from its key, value, tag or first child.
    int nodeLine(ryml::id_type id) const
    {
        int result = -1;
        if (m_tree.has_key(id))
        {
            result = line(m_tree.key(id).str);
        }
        if (result < 0 && m_tree.has_val(id))
        {
            result = line(m_tree.val(id).str);
        }
        if (result < 0 && m_tree.has_val_tag(id))
        {
            result = line(m_tree.val_tag(id).str);
        }
        if (result < 0 && m_tree.has_children(id))
        {
            result = nodeLine(m_tree.first_child(id));
        }
        return result;
    }

    std::string m_source;
    ryml::EventHandlerTree m_handler;
    ryml::Parser m_parser;
    ryml::Tree m_tree;
    size_t m_root = size_t(ryml::NONE);
    // The stream has no documents, the root is a null node.
    bool m_empty = false;
};

namespace
{

// Remove the brackets of verbatim tags, e.g. <ColorSpace>.
std::string NormalizeTag(ryml::csubstr tag)
{
    if (tag.len >= 2 && tag.str[0] == '<' && tag.str[tag.len - 1] == '>')
    {
        return std::string(tag.str + 1, tag.len - 2);
    }
    return std::string(tag.str, tag.len);
}

[[noreturn]] void ThrowBadConversion(const Node & node)
{
    std::ostringstream os;
    os << "error at line " << (node.Mark().line + 1) << ": bad conversion";
    throw Exception(os.str().c_str());
}

bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Parse a number like yaml-cpp does with iostreams: decimal notation only, no leading
// whitespace, trailing whitespace allowed, plus the YAML infinity and NaN forms.
template<typename T>
bool ParseNumber(const std::string & str, T & value)
{
    if (str == ".inf" || str == ".Inf" || str == ".INF" ||
        str == "+.inf" || str == "+.Inf" || str == "+.INF")
    {
        value = std::numeric_limits<T>::infinity();
        return true;
    }
    if (str == "-.inf" || str == "-.Inf" || str == "-.INF")
    {
        value = -std::numeric_limits<T>::infinity();
        return true;
    }
    if (str == ".nan" || str == ".NaN" || str == ".NAN")
    {
        value = std::numeric_limits<T>::quiet_NaN();
        return true;
    }

    if (str.empty() || IsSpace(str[0]))
    {
        return false;
    }

    const char * first = str.data();
    const char * last = first + str.size();
    while (last > first && IsSpace(last[-1]))
    {
        --last;
    }

    for (const char * p = first; p < last; ++p)
    {
        const char c = *p;
        if (!((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-'))
        {
            return false;
        }
    }

    const auto result = NumberUtils::from_chars(first, last, value);
    return result.ec == std::errc() && result.ptr == last;
}

// Parse a boolean like yaml-cpp: y/n, yes/no, true/false and on/off, in lowercase, uppercase
// or capitalized form.
bool ParseBool(const std::string & str, bool & value)
{
    auto isLower = [](char c) { return c >= 'a' && c <= 'z'; };
    auto isUpper = [](char c) { return c >= 'A' && c <= 'Z'; };
    auto allOf = [](const char * first, const char * last, bool (*pred)(char))
    {
        return std::all_of(first, last, pred);
    };

    // Must be lowercase, uppercase or capitalized.
    const char * first = str.data();
    const char * last = first + str.size();
    if (!str.empty() && !allOf(first, last, isLower))
    {
        if (!isUpper(str[0]) ||
            !(allOf(first + 1, last, isLower) || allOf(first + 1, last, isUpper)))
        {
            return false;
        }
    }

    static const char * names[][2] = {
        { "y", "n" }, { "yes", "no" }, { "true", "false" }, { "on", "off" } };

    const std::string lower = StringUtils::Lower(str);
    for (const auto & name : names)
    {
        if (lower == name[0])
        {
            value = true;
            return true;
        }
        if (lower == name[1])
        {
            value = false;
            return true;
        }
    }
    return false;
}

} // namespace

NodeType::value Node::Type() const
{
    if (!m_doc || m_id == size_t(ryml::NONE))
    {
        return NodeType::Undefined;
    }

    const ryml::Tree & tree = m_doc->m_tree;
    const ryml::id_type id = ryml::id_type(m_id);
    if (m_isKey)
    {
        return tree.key_is_null(id) ? NodeType::Null : NodeType::Scalar;
    }
    if (m_id == m_doc->m_root && m_doc->m_empty)
    {
        return NodeType::Null;
    }
    if (tree.is_map(id))
    {
        return NodeType::Map;
    }
    if (tree.is_seq(id))
    {
        return NodeType::Sequence;
    }
    if (tree.has_val(id) && !tree.val_is_null(id))
    {
        return NodeType::Scalar;
    }
    return NodeType::Null;
}

std::string Node::Tag() const
{
    const NodeType::value type = Type();
    if (type == NodeType::Undefined)
    {
        return "";
    }

    const ryml::Tree & tree = m_doc->m_tree;
    const ryml::id_type id = ryml::id_type(m_id);
    if (m_isKey ? tree.has_key_tag(id) : tree.has_val_tag(id))
    {
        return NormalizeTag(m_isKey ? tree.key_tag(id) : tree.val_tag(id));
    }

    switch (type)
    {
        case NodeType::Undefined:
        case NodeType::Null:
            return "";
        case NodeType::Scalar:
            return (m_isKey ? tree.is_key_quoted(id) : tree.is_val_quoted(id)) ? "!" : "?";
        case NodeType::Sequence:
        case NodeType::Map:
            break;
    }
    return "?";
}

Mark Node::Mark() const
{
    YamlRead::Mark mark;
    if (Type() == NodeType::Undefined || (m_id == m_doc->m_root && Type() == NodeType::Null))
    {
        return mark;
    }

    const ryml::Tree & tree = m_doc->m_tree;
    const ryml::id_type id = ryml::id_type(m_id);
    if (m_isKey)
    {
        mark.line = m_doc->line(tree.key(id).str);
        return mark;
    }

    // Try the value itself, then its tag, then the first child.
    if (tree.has_val(id) && tree.val(id).str)
    {
        mark.line = m_doc->line(tree.val(id).str);
    }
    if (mark.line < 0 && tree.has_val_tag(id))
    {
        mark.line = m_doc->line(tree.val_tag(id).str);
    }
    if (mark.line < 0 && tree.has_children(id))
    {
        mark.line = m_doc->nodeLine(tree.first_child(id));
    }
    if (mark.line < 0 && tree.has_key(id))
    {
        mark.line = m_doc->line(tree.key(id).str);
    }
    return mark;
}

size_t Node::size() const
{
    const NodeType::value type = Type();
    if (type != NodeType::Map && type != NodeType::Sequence)
    {
        return 0;
    }
    return m_doc->m_tree.num_children(ryml::id_type(m_id));
}

std::string Node::scalar() const
{
    const ryml::Tree & tree = m_doc->m_tree;
    const ryml::csubstr str = m_isKey ? tree.key(ryml::id_type(m_id))
                                      : tree.val(ryml::id_type(m_id));
    return std::string(str.str ? str.str : "", str.len);
}

template<>
std::string Node::as<std::string>() const
{
    switch (Type())
    {
        case NodeType::Undefined:
            throw Exception("invalid node; this may result from using a map iterator as a "
                            "sequence iterator, or vice-versa");
        case NodeType::Null:
            return "null";
        case NodeType::Scalar:
            return scalar();
        case NodeType::Sequence:
        case NodeType::Map:
            break;
    }
    ThrowBadConversion(*this);
}

template<>
bool Node::as<bool>() const
{
    bool value = false;
    if (Type() != NodeType::Scalar || !ParseBool(scalar(), value))
    {
        ThrowBadConversion(*this);
    }
    return value;
}

template<>
double Node::as<double>() const
{
    double value = 0.0;
    if (Type() != NodeType::Scalar || !ParseNumber(scalar(), value))
    {
        ThrowBadConversion(*this);
    }
    return value;
}

template<>
float Node::as<float>() const
{
    float value = 0.0f;
    if (Type() != NodeType::Scalar || !ParseNumber(scalar(), value))
    {
        ThrowBadConversion(*this);
    }
    return value;
}

namespace
{

template<typename T>
std::vector<T> AsVector(const Node & node)
{
    if (node.Type() != NodeType::Sequence)
    {
        ThrowBadConversion(node);
    }

    std::vector<T> values;
    values.reserve(node.size());
    for (const auto & element : node)
    {
        values.push_back(element.as<T>());
    }
    return values;
}

} // namespace

template<>
std::vector<std::string> Node::as<std::vector<std::string>>() const
{
    return AsVector<std::string>(*this);
}

template<>
std::vector<float> Node::as<std::vector<float>>() const
{
    return AsVector<float>(*this);
}

template<>
std::vector<double> Node::as<std::vector<double>>() const
{
    return AsVector<double>(*this);
}

Node Node::operator[](const char * key) const
{
    if (Type() != NodeType::Map)
    {
        return Node();
    }

    const ryml::id_type child = m_doc->m_tree.find_child(ryml::id_type(m_id),
                                                         ryml::to_csubstr(key));
    if (child == ryml::NONE)
    {
        return Node();
    }
    return Node(m_doc, child, false);
}

Node Node::operator[](size_t index) const
{
    if (Type() != NodeType::Sequence)
    {
        return Node();
    }

    const ryml::id_type child = m_doc->m_tree.child(ryml::id_type(m_id), ryml::id_type(index));
    if (child == ryml::NONE)
    {
        return Node();
    }
    return Node(m_doc, child, false);
}

ConstIterator Node::begin() const
{
    const NodeType::value type = Type();
    if (type != NodeType::Map && type != NodeType::Sequence)
    {
        return end();
    }
    return ConstIterator(m_doc, m_doc->m_tree.first_child(ryml::id_type(m_id)),
                         type == NodeType::Map);
}

ConstIterator Node::end() const
{
    return ConstIterator(m_doc, size_t(ryml::NONE), false);
}

ConstIterator::ConstIterator(const std::shared_ptr<const Document> & doc, size_t id, bool isMap)
    : m_doc(doc), m_id(id), m_isMap(isMap)
{
    update();
}

ConstIterator & ConstIterator::operator++()
{
    m_id = m_doc->m_tree.next_sibling(ryml::id_type(m_id));
    update();
    return *this;
}

void ConstIterator::update()
{
    m_value = IteratorValue();
    if (m_id == size_t(ryml::NONE))
    {
        return;
    }

    if (m_isMap)
    {
        m_value.first = Node(m_doc, m_id, true);
        m_value.second = Node(m_doc, m_id, false);
    }
    else
    {
        static_cast<Node &>(m_value) = Node(m_doc, m_id, false);
    }
}

Node Load(std::istream & istream)
{
    auto doc = std::make_shared<Document>();

    // Read in blocks, much faster than per character. Read from the stream buffer directly
    // like yaml-cpp, ignoring the stream state flags.
    std::string & source = doc->m_source;
    std::streambuf * streamBuffer = istream.rdbuf();
    char buffer[16384];
    std::streamsize size = 0;
    while (streamBuffer && (size = streamBuffer->sgetn(buffer, sizeof(buffer))) > 0)
    {
        source.append(buffer, size_t(size));
    }

    // Parse in place, the tree refers to the source string owned by the document.
    ryml::parse_in_place(&doc->m_parser, ryml::to_substr(source), &doc->m_tree);

    // Use the first document of a stream.
    const ryml::Tree & tree = doc->m_tree;
    ryml::id_type root = tree.root_id();
    if (tree.is_stream(root))
    {
        if (tree.has_children(root))
        {
            root = tree.first_child(root);
        }
        else
        {
            doc->m_empty = true;
        }
    }
    doc->m_root = size_t(root);

    return Node(doc, size_t(root), false);
}

} // namespace YamlRead

} // namespace OCIO_NAMESPACE
