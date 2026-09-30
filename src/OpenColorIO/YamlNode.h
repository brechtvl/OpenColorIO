// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the OpenColorIO Project.

#ifndef INCLUDED_OCIO_YAMLNODE_H
#define INCLUDED_OCIO_YAMLNODE_H

#include <istream>
#include <memory>
#include <string>

#include <OpenColorIO/OpenColorIO.h>

namespace OCIO_NAMESPACE
{

// Read-only YAML node tree, backed by rapidyaml. Mirrors the subset of the yaml-cpp Node API
// used for reading configs, with the same conversion and tag semantics.
namespace YamlRead
{

struct NodeType
{
    enum value { Undefined, Null, Scalar, Sequence, Map };
};

// Position in the source text. Lines are zero-based, -1 if unknown.
struct Mark
{
    int line = -1;
};

class Document;
class ConstIterator;

class Node
{
public:
    Node() = default;

    NodeType::value Type() const;
    bool IsDefined() const { return Type() != NodeType::Undefined; }
    bool IsNull() const { return Type() == NodeType::Null; }

    // Explicit tag, with the verbatim form !<Name> returned as Name. Untagged nodes return "?"
    // for plain scalars and collections, "!" for other scalars and "" for null.
    std::string Tag() const;
    YamlRead::Mark Mark() const;

    // Number of children for collections, 0 otherwise.
    size_t size() const;

    // Conversions, throwing an Exception on failure. Supported types are bool, float, double,
    // std::string and std::vector of those.
    template<typename T> T as() const;

    // Child of a map by key, or an undefined node if there is none.
    Node operator[](const char * key) const;
    // Element of a sequence, or an undefined node if there is none.
    Node operator[](size_t index) const;

    ConstIterator begin() const;
    ConstIterator end() const;

private:
    friend class ConstIterator;
    friend Node Load(std::istream & istream);

    Node(const std::shared_ptr<const Document> & doc, size_t id, bool isKey)
        : m_doc(doc), m_id(id), m_isKey(isKey)
    {
    }

    std::string scalar() const;

    std::shared_ptr<const Document> m_doc;
    size_t m_id = size_t(-1);
    // Refers to the key of the map entry m_id, rather than its value.
    bool m_isKey = false;
};

// Value of an iterator. For sequences it is the element itself, for maps first and second are
// the key and value.
struct IteratorValue : public Node
{
    Node first;
    Node second;
};

class ConstIterator
{
public:
    const IteratorValue & operator*() const { return m_value; }
    const IteratorValue * operator->() const { return &m_value; }
    ConstIterator & operator++();
    bool operator==(const ConstIterator & other) const { return m_id == other.m_id; }
    bool operator!=(const ConstIterator & other) const { return m_id != other.m_id; }

private:
    friend class Node;

    ConstIterator(const std::shared_ptr<const Document> & doc, size_t id, bool isMap);
    void update();

    std::shared_ptr<const Document> m_doc;
    size_t m_id = size_t(-1);
    bool m_isMap = false;
    IteratorValue m_value;
};

// Parse the first document of a YAML stream. Throws an Exception on syntax errors.
Node Load(std::istream & istream);

} // namespace YamlRead

} // namespace OCIO_NAMESPACE

#endif // INCLUDED_OCIO_YAMLNODE_H
