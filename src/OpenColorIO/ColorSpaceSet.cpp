// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the OpenColorIO Project.

#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

#include <OpenColorIO/OpenColorIO.h>

#include "PrivateTypes.h"
#include "utils/StringUtils.h"


namespace OCIO_NAMESPACE
{

// Case insensitive map from color space names and aliases to indices.
// Keys point to strings owned by the color spaces, so a color space
// must be removed before it is destroyed.
class ColorSpaceIndexMap
{
public:
    int find(const char * name) const
    {
        const auto it = m_map.find(name);
        return (it != m_map.end()) ? it->second : -1;
    }

    void add(const ColorSpace & cs, int idx)
    {
        m_map.emplace(cs.getName(), idx);
        const size_t numAliases = cs.getNumAliases();
        for (size_t aidx = 0; aidx < numAliases; ++aidx)
        {
            m_map.emplace(cs.getAlias(aidx), idx);
        }
    }

    void remove(const ColorSpace & cs, int idx)
    {
        removeKey(cs.getName(), idx);
        const size_t numAliases = cs.getNumAliases();
        for (size_t aidx = 0; aidx < numAliases; ++aidx)
        {
            removeKey(cs.getAlias(aidx), idx);
        }
    }

    void rebuild(const std::vector<ColorSpaceRcPtr> & colorSpaces)
    {
        m_map.clear();
        for (size_t idx = 0; idx < colorSpaces.size(); ++idx)
        {
            add(*colorSpaces[idx], static_cast<int>(idx));
        }
    }

    void clear()
    {
        m_map.clear();
    }

private:
    struct CaseInsensitiveHash
    {
        size_t operator()(std::string_view str) const noexcept
        {
            return StringUtils::Hash(str);
        }
    };

    struct CaseInsensitiveEqual
    {
        bool operator()(std::string_view a, std::string_view b) const noexcept
        {
            return StringUtils::Compare(a, b);
        }
    };

    void removeKey(const char * key, int idx)
    {
        // Only remove the entry if it belongs to this index, in case another color space
        // uses the same name.
        const auto it = m_map.find(key);
        if (it != m_map.end() && it->second == idx)
        {
            m_map.erase(it);
        }
    }

    std::unordered_map<std::string_view, int, CaseInsensitiveHash, CaseInsensitiveEqual> m_map;
};

class ColorSpaceSet::Impl
{
public:
    Impl() = default;
    ~Impl() = default;

    Impl(const Impl &) = delete;

    Impl & operator= (const Impl & rhs)
    {
        if (this != &rhs)
        {
            clear();

            for (auto & cs: rhs.m_colorSpaces)
            {
                m_colorSpaces.push_back(cs->createEditableCopy());
            }
            m_indexMap.rebuild(m_colorSpaces);
        }
        return *this;
    }

    bool operator== (const Impl & rhs) const
    {
        if (this == &rhs) return true;

        if (m_colorSpaces.size() != rhs.m_colorSpaces.size())
        {
            return false;
        }

        for (auto & cs : m_colorSpaces)
        {
            // NB: Only the names are compared.
            if (!rhs.isPresent(cs->getName()))
            {
                return false;
            }
        }

        return true;
    }

    int size() const 
    { 
        return static_cast<int>(m_colorSpaces.size()); 
    }

    ConstColorSpaceRcPtr get(int index) const 
    {
        if (index < 0 || index >= size())
        {
            return ColorSpaceRcPtr();
        }

        return m_colorSpaces[index];
    }

    const char * getName(int index) const 
    {
        if (index < 0 || index >= size())
        {
            return nullptr;
        }

        return m_colorSpaces[index]->getName();
    }

    ConstColorSpaceRcPtr getByName(const char * csName) const 
    {
        return get(getIndex(csName));
    }

    int getIndex(const char * csName) const 
    {
        // Search for name and aliases.
        if (csName && *csName)
        {
            return m_indexMap.find(csName);
        }

        return -1;
    }

    bool isPresent(const char * csName) const
    {
        return -1 != getIndex(csName);
    }

    void add(const ConstColorSpaceRcPtr & cs)
    {
        const char * csName = cs->getName();
        if (!*csName)
        {
            throw Exception("Cannot add a color space with an empty name.");
        }

        auto entryIdx = getIndex(csName);
        size_t replaceIdx = (size_t)-1;
        if (entryIdx != -1)
        {
            // If getIndex succeeds but the csName is not the name of the matching color space, it
            // means that csName must be an alias name.  Color space will be replaced only when
            // canonical names match.
            if (!StringUtils::Compare(m_colorSpaces[entryIdx]->getName(), csName))
            {
                std::ostringstream os;
                os << "Cannot add '" << csName << "' color space, existing color space, '";
                os << m_colorSpaces[entryIdx]->getName() << "' is using this name as an alias.";
                throw Exception(os.str().c_str());
            }
            // There is a color space with the same name that will be replaced (if new color space
            // can be used).
            replaceIdx = entryIdx;
        }

        const size_t numAliases = cs->getNumAliases();
        for (size_t aidx = 0; aidx < numAliases; ++aidx)
        {
            const char * alias = cs->getAlias(aidx);
            entryIdx = getIndex(alias);
            // Is an alias of the color space already used by a color space?
            // Skip existing colorspace that might be replaced.
            if (entryIdx != -1 && static_cast<int>(replaceIdx) != entryIdx)
            {
                std::ostringstream os;
                os << "Cannot add '" << csName << "' color space, it has '" << alias;
                os << "' alias and existing color space, '";
                os << m_colorSpaces[entryIdx]->getName() << "' is using the same alias.";
                throw Exception(os.str().c_str());
            }
        }
        if (replaceIdx != (size_t)-1)
        {
            // The color space replaces the existing one.
            const int idx = static_cast<int>(replaceIdx);
            m_indexMap.remove(*m_colorSpaces[replaceIdx], idx);
            m_colorSpaces[replaceIdx] = cs->createEditableCopy();
            m_indexMap.add(*m_colorSpaces[replaceIdx], idx);
            return;
        }

        m_colorSpaces.push_back(cs->createEditableCopy());
        m_indexMap.add(*m_colorSpaces.back(), static_cast<int>(m_colorSpaces.size() - 1));
    }

    void add(const Impl & rhs)
    {
        for (auto & cs : rhs.m_colorSpaces)
        {
            add(cs);
        }
    }

    void remove(const char * csName)
    {
        if (!csName || !*csName) return;

        for (auto itr = m_colorSpaces.begin(); itr != m_colorSpaces.end(); ++itr)
        {
            if (StringUtils::Compare((*itr)->getName(), csName))
            {
                m_colorSpaces.erase(itr);
                m_indexMap.rebuild(m_colorSpaces);
                return;
            }
        }
    }

    void remove(const Impl & rhs)
    {
        for (auto & cs : rhs.m_colorSpaces)
        {
            remove(cs->getName());
        }
    }

    void clear()
    {
        m_colorSpaces.clear();
        m_indexMap.clear();
    }

private:
    typedef std::vector<ColorSpaceRcPtr> ColorSpaceVec;
    ColorSpaceVec m_colorSpaces;
    ColorSpaceIndexMap m_indexMap;
};


///////////////////////////////////////////////////////////////////////////

ColorSpaceSetRcPtr ColorSpaceSet::Create()
{
    return ColorSpaceSetRcPtr(new ColorSpaceSet(), &deleter);
}

void ColorSpaceSet::deleter(ColorSpaceSet* c)
{
    delete c;
}


///////////////////////////////////////////////////////////////////////////



ColorSpaceSet::ColorSpaceSet()
    :   m_impl(new ColorSpaceSet::Impl)
{
}

ColorSpaceSet::~ColorSpaceSet()
{
    delete m_impl;
    m_impl = nullptr;
}

ColorSpaceSetRcPtr ColorSpaceSet::createEditableCopy() const
{
    ColorSpaceSetRcPtr css = ColorSpaceSet::Create();
    *css->m_impl = *m_impl; // Deep Copy.
    return css;
}

bool ColorSpaceSet::operator==(const ColorSpaceSet & css) const
{
    return *m_impl == *css.m_impl;
}

bool ColorSpaceSet::operator!=(const ColorSpaceSet & css) const
{
    return !( *m_impl == *css.m_impl );
}

int ColorSpaceSet::getNumColorSpaces() const
{
    return m_impl->size();    
}

const char * ColorSpaceSet::getColorSpaceNameByIndex(int index) const
{
    return m_impl->getName(index);
}

ConstColorSpaceRcPtr ColorSpaceSet::getColorSpaceByIndex(int index) const
{
    return m_impl->get(index);
}

ConstColorSpaceRcPtr ColorSpaceSet::getColorSpace(const char * name) const
{
    return m_impl->getByName(name);
}

int ColorSpaceSet::getColorSpaceIndex(const char * name) const
{
    return m_impl->getIndex(name);
}

bool ColorSpaceSet::hasColorSpace(const char * name) const
{
    return m_impl->isPresent(name);
}

void ColorSpaceSet::addColorSpace(const ConstColorSpaceRcPtr & cs)
{
    return m_impl->add(cs);
}

void ColorSpaceSet::addColorSpaces(const ConstColorSpaceSetRcPtr & css)
{
    return m_impl->add(*css->m_impl);
}

void ColorSpaceSet::removeColorSpace(const char * name)
{
    return m_impl->remove(name);
}

void ColorSpaceSet::removeColorSpaces(const ConstColorSpaceSetRcPtr & css)
{
    return m_impl->remove(*css->m_impl);
}

void ColorSpaceSet::clearColorSpaces()
{
    m_impl->clear();
}

ConstColorSpaceSetRcPtr operator||(const ConstColorSpaceSetRcPtr & lcss, 
                                   const ConstColorSpaceSetRcPtr & rcss)
{
    ColorSpaceSetRcPtr css = lcss->createEditableCopy();
    css->addColorSpaces(rcss);
    return css;    
}

ConstColorSpaceSetRcPtr operator&&(const ConstColorSpaceSetRcPtr & lcss, 
                                   const ConstColorSpaceSetRcPtr & rcss)
{
    ColorSpaceSetRcPtr css = ColorSpaceSet::Create();

    for (int idx = 0; idx < rcss->getNumColorSpaces(); ++idx)
    {
        ConstColorSpaceRcPtr tmp = rcss->getColorSpaceByIndex(idx);
        if (lcss->hasColorSpace(tmp->getName()))
        {
            css->addColorSpace(tmp);
        }
    }

    return css;
}

ConstColorSpaceSetRcPtr operator-(const ConstColorSpaceSetRcPtr & lcss, 
                                  const ConstColorSpaceSetRcPtr & rcss)
{
    ColorSpaceSetRcPtr css = ColorSpaceSet::Create();

    for (int idx = 0; idx < lcss->getNumColorSpaces(); ++idx)
    {
        ConstColorSpaceRcPtr tmp = lcss->getColorSpaceByIndex(idx);

        if (!rcss->hasColorSpace(tmp->getName()))
        {
            css->addColorSpace(tmp);
        }
    }

    return css;
}

} // namespace OCIO_NAMESPACE

