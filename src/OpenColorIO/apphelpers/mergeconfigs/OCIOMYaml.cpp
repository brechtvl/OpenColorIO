// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the OpenColorIO Project.

#include <cstring>
#include <unordered_set>
#include <iostream>

#include <pystring.h>

#include <OpenColorIO/OpenColorIO.h>

#include "Logging.h"
#include "OCIOMYaml.h"
#include "ParseUtils.h"
#include "PathUtils.h"
#include "utils/StringUtils.h"
#include "YamlEmitter.h"

namespace OCIO_NAMESPACE
{

std::unordered_map<std::string, ConfigMergingParameters::MergeStrategies> OCIOMYaml::m_mergeStrategiesMap;

OCIOMYaml::OCIOMYaml()
{
    if (m_mergeStrategiesMap.empty())
    {
        m_mergeStrategiesMap["PreferInput"] = ConfigMergingParameters::MergeStrategies::STRATEGY_PREFER_INPUT;
        m_mergeStrategiesMap["PreferBase"] = ConfigMergingParameters::MergeStrategies::STRATEGY_PREFER_BASE;
        m_mergeStrategiesMap["InputOnly"] = ConfigMergingParameters::MergeStrategies::STRATEGY_INPUT_ONLY;
        m_mergeStrategiesMap["BaseOnly"] = ConfigMergingParameters::MergeStrategies::STRATEGY_BASE_ONLY;
        m_mergeStrategiesMap["Remove"] = ConfigMergingParameters::MergeStrategies::STRATEGY_REMOVE;
    }
}

void OCIOMYaml::load(const YamlRead::Node & node, std::string & x)
{
    try
    {
        x = node.as<std::string>();
    }
    catch (const std::exception & e)
    {
        std::ostringstream os;
        os << "At line " << (node.Mark().line + 1)
            << ", '" << node.Tag() << "' parsing string failed "
            << "with: " << e.what();
        throw Exception(os.str().c_str());
    }
}

void OCIOMYaml::load(const YamlRead::Node & node, std::vector<std::string> & x)
{
    try
    {
        x = node.as<std::vector<std::string>>();
    }
    catch (const std::exception & e)
    {
        std::ostringstream os;
        os << "At line " << (node.Mark().line + 1)
           << ", '" << node.Tag() << "' parsing StringVec failed "
           << "with: " << e.what();
        throw Exception(os.str().c_str());
    }
}

void OCIOMYaml::throwValueError(const std::string & nodeName,
                                       const YamlRead::Node & key,
                                       const std::string & msg)
{
    std::string keyName;
    load(key, keyName);

    std::ostringstream os;
    os << "At line " << (key.Mark().line + 1)
        << ", the value of the property '" << keyName 
        << "' from '" << nodeName << "' failed: " << msg;

    throw Exception(os.str().c_str());
}

void OCIOMYaml::CheckDuplicates(const YamlRead::Node & node)
{
    std::unordered_set<std::string> keyset;

    for (YamlRead::ConstIterator iter = node.begin(); iter != node.end(); ++iter)
    {
        const std::string & key = iter->first.as<std::string>();
        if (keyset.find(key) == keyset.end())
        {
            keyset.insert(key);
        }
        else
        {
            std::ostringstream os;
            os << "Key-value pair with key '" << key;
            os << "' specified more than once. ";
            throwValueError(node.Tag(), iter->first, os.str());
        }
    }
}

ConfigMergingParameters::MergeStrategies OCIOMYaml::genericStrategyHandler(const YamlRead::Node & pnode, const YamlRead::Node & node)
{
    if(node.Type() != YamlRead::NodeType::Map)
    {
        throwValueError(node.Tag(), pnode,
                        "The value type of a property 'strategy' needs to be a map.");
    }
    
    std::string strategy;
    for (YamlRead::ConstIterator it = node.begin(); it != node.end(); ++it)
    {
        const std::string & prop = it->first.as<std::string>();
        const std::string & value = it->second.as<std::string>();

        if (prop == "strategy")
        {
            strategy = value;
        }
    }

    auto srategyEnum = OCIOMYaml::StrategyStringToEnum(strategy.c_str());
    if (srategyEnum == ConfigMergingParameters::MergeStrategies::STRATEGY_UNSPECIFIED)
    {
        std::ostringstream os;
        os << "The value '" << strategy;
        os << "' is not recognized. ";
        throwValueError(node.Tag(), pnode, os.str());
    }
    return srategyEnum;
}

void OCIOMYaml::loadOptions(const YamlRead::Node & node, ConfigMergingParametersRcPtr & params)
{
    CheckDuplicates(node);

    for (YamlRead::ConstIterator it = node.begin(); it != node.end(); ++it)
    {
        const std::string & key = it->first.as<std::string>();
        if (key == "input_family_prefix")
        {
            params->setInputFamilyPrefix(it->second.as<std::string>().c_str());
        }
        else if (key == "base_family_prefix")
        {
           params->setBaseFamilyPrefix(it->second.as<std::string>().c_str());
        }
        else if (key == "input_first")
        {
            params->setInputFirst(it->second.as<bool>());
        }
        else if (key == "error_on_conflict")
        {
            params->setErrorOnConflict(it->second.as<bool>());
        }
        else if (key == "avoid_duplicates")
        {
            params->setAvoidDuplicates(it->second.as<bool>());
        }
        else if (key == "adjust_input_reference_space")
        {
            params->setAdjustInputReferenceSpace(it->second.as<bool>());
        }
        else if (key == "assume_common_reference_space")
        {
            // Need to support this as a synonym for adjust_input_reference_space.
            params->setAdjustInputReferenceSpace(!it->second.as<bool>());
        }
        else if (key == "default_strategy")
        {
            const std::string & strategy = it->second.as<std::string>();
            auto srategyEnum = OCIOMYaml::StrategyStringToEnum(strategy.c_str());
            if (srategyEnum == ConfigMergingParameters::MergeStrategies::STRATEGY_UNSPECIFIED)
            {
                std::ostringstream os;
                os << "The value '" << strategy;
                os << "' is not recognized. ";
                throwValueError(node.Tag(), it->first, os.str());
            }
            params->setDefaultStrategy(srategyEnum);
        }
    }
}

void OCIOMYaml::loadOverrides(const YamlRead::Node & node, ConfigMergingParametersRcPtr & params)
{
    CheckDuplicates(node);

    std::string stringval;

    for (YamlRead::ConstIterator it = node.begin(); it != node.end(); ++it)
    {
        const std::string & key = it->first.as<std::string>();

        if (it->second.IsNull() || !it->second.IsDefined()) continue;

        if (key == "name")
        {
            load(it->second, stringval);
            params->setName(stringval.c_str());
        }
        else if (key == "description")
        {
            load(it->second, stringval);
            params->setDescription(stringval.c_str());
        }
        else if (key == "search_path")
        {
            if (it->second.size() == 0)
            {
                load(it->second, stringval);
                params->setSearchPath(stringval.c_str());
            }
            else
            {
                std::vector<std::string> paths;
                load(it->second, paths);
                for (const auto & path : paths)
                {
                    params->addSearchPath(path.c_str());
                }
            }
        }
        else if (key == "environment")
        {
            if(it->second.Type() != YamlRead::NodeType::Map)
            {
                throwValueError(node.Tag(), it->first, 
                                "The value type of key 'environment' needs to be a map.");
            }
            for (YamlRead::ConstIterator itEnv = it->second.begin(); itEnv != it->second.end(); ++itEnv)
            {
                const std::string & k = itEnv->first.as<std::string>();
                const std::string & v = itEnv->second.as<std::string>();
                params->addEnvironmentVar(k.c_str(), v.c_str());
            }
        }
        else if (key == "active_displays")
        {
            std::vector<std::string> display;
            load(it->second, display);
            std::string displays = StringUtils::Join(display, ',');
            params->setActiveDisplays(displays.c_str());
        }
        else if (key == "active_views")
        {
            std::vector<std::string> view;
            load(it->second, view);
            std::string views = StringUtils::Join(view, ',');
            params->setActiveViews(views.c_str());
        }
        else if (key == "inactive_colorspaces")
        {
            std::vector<std::string> inactiveCSs;
            load(it->second, inactiveCSs);
            const std::string inactivecCSsStr = StringUtils::Join(inactiveCSs, ',');
            params->setInactiveColorSpaces(inactivecCSsStr.c_str());
        }
    }
}

void OCIOMYaml::loadParams(const YamlRead::Node & node, ConfigMergingParametersRcPtr & params)
{
    // Check for duplicates in params.
    CheckDuplicates(node);

    for (YamlRead::ConstIterator it = node.begin(); it != node.end(); ++it)
    {
        const std::string & key = it->first.as<std::string>();
        if (key == "roles")
        {
            params->setRoles(genericStrategyHandler(it->first, it->second));
        }
        else if (key == "file_rules")
        {
            params->setFileRules(genericStrategyHandler(it->first, it->second));
        }
        else if (key == "display-views")
        {
            params->setDisplayViews(genericStrategyHandler(it->first, it->second));
        }  
        else if (key == "view_transforms")
        {
            params->setViewTransforms(genericStrategyHandler(it->first, it->second));
        }  
        else if (key == "looks")
        {
            params->setLooks(genericStrategyHandler(it->first, it->second));
        }              
        else if (key == "colorspaces")
        {
            params->setColorspaces(genericStrategyHandler(it->first, it->second));
        }   
        else if (key == "named_transforms")
        {
            params->setNamedTransforms(genericStrategyHandler(it->first, it->second));
        }                                                          
        else
        {
            LogWarning("Unsupported property in merge params: " + key);
        }
    }
}

void OCIOMYaml::load(const YamlRead::Node& node, ConfigMergerRcPtr & merger, const char * filename)
{
    CheckDuplicates(node);

    // Parse all properties.
    for (YamlRead::ConstIterator it = node.begin(); it != node.end(); ++it)
    {
        const std::string & key = it->first.as<std::string>();

        if (it->second.IsNull() || !it->second.IsDefined()) continue;

        if (key == "ociom_version")
        {
            std::string version;
            std::vector<std::string> results;

            load(node["ociom_version"], version);
            results = StringUtils::Split(version, '.');

            bool faulty_version = false;
            try
            {
                if(results.size() == 1)
                {
                    merger->setVersion(std::stoi(results[0].c_str()), 0);
                }
                else if(results.size() == 2)
                {
                    merger->setVersion(std::stoi(results[0].c_str()),
                                       std::stoi(results[1].c_str()));
                }
                else
                {
                    faulty_version = true;
                }
            }
            catch (const std::exception &)
            {
                faulty_version = true;
            }

            if (faulty_version)
            {
                throwValueError(it->second.Tag(), it->first,
                                "The value '" + version + "' is not a valid OCIOM version.");
            }

            if (merger->getMajorVersion() > 1u || merger->getMinorVersion() > 0u)
            {
                throwValueError(it->second.Tag(), it->first,
                                "The highest supported OCIOM file version is 1.0.");
            }
        }
        else if (key == "search_path")
        {
            if (it->second.size() == 0)
            {
                std::string stringval;
                load(it->second, stringval);
                merger->setSearchPath(stringval.c_str());
            }
            else
            {
                StringUtils::StringVec paths;
                load(it->second, paths);
                for (const auto & path : paths)
                {
                    merger->addSearchPath(path.c_str());
                }
            }
        }
        else if (key == "merge")
        {
            if(it->second.Type() != YamlRead::NodeType::Map)
            {
                throwValueError(it->second.Tag(), it->first,
                                "The value type of the key 'merge' needs to be a map.");
            }

            int mergesCounter = 0;
            for (YamlRead::ConstIterator mergeIt = it->second.begin(); mergeIt != it->second.end(); ++mergeIt)
            {
                const std::string & mergedConfigName = mergeIt->first.as<std::string>();
                ConfigMergingParametersRcPtr params = merger->getParams(mergesCounter);
                params->setOutputName(mergedConfigName.c_str());

                for (YamlRead::ConstIterator paramsIt = mergeIt->second.begin(); paramsIt != mergeIt->second.end(); ++paramsIt)
                {
                    const std::string & key = paramsIt->first.as<std::string>();
                    
                    if (key == "base")
                    {
                        params->setBaseConfigName(paramsIt->second.as<std::string>().c_str());
                    }
                    else if (key == "input")
                    {
                        params->setInputConfigName(paramsIt->second.as<std::string>().c_str());
                    }
                    else if (key == "options")
                    {
                        loadOptions(paramsIt->second, params);
                    }
                    else if (key == "overrides")
                    {
                        loadOverrides(paramsIt->second, params);
                    }
                    else if (key == "params")
                    {
                        loadParams(paramsIt->second, params);
                    }
                }
                mergesCounter++;
            }
        }

        if (filename && filename[0])
        {
            // Working directory defaults to the directory of the OCIOM file.
            std::string realfilename = AbsPath(filename);
            std::string configrootdir = pystring::os::path::dirname(realfilename);
            merger->setWorkingDir(configrootdir.c_str());
        }
    }    
}

int OCIOMYaml::countMerges(const YamlRead::Node& node)
{
    int numOfMerges = 0;

    CheckDuplicates(node);

    // Parse all properties.
    for (YamlRead::ConstIterator it = node.begin(); it != node.end(); ++it)
    {
        const std::string & key = it->first.as<std::string>();

        if (it->second.IsNull() || !it->second.IsDefined()) continue;

        if (key == "merge")
        {
            if(it->second.Type() != YamlRead::NodeType::Map)
            {
                throwValueError(it->second.Tag(), it->first,
                                "The value type of the key 'merge' needs to be a map.");
            }

            CheckDuplicates(it->second);

            for (YamlRead::ConstIterator mergeIt = it->second.begin(); mergeIt != it->second.end(); ++mergeIt)
            {
                numOfMerges++;
            }
        }
    }

    return numOfMerges;
}

void OCIOMYaml::read(std::istream & istream, ConfigMergerRcPtr & merger, const char * filepath)
{
    try
    {
        YamlRead::Node node = YamlRead::Load(istream);
        load(node, merger, filepath);
    }
    catch(const std::exception & e)
    {
        std::ostringstream os;
        os << "Error: Loading the OCIOM Merge parameters ";
        os << "'" << filepath << "' failed. " << e.what();
        throw Exception(os.str().c_str());
    }
}

ConfigMergingParameters::MergeStrategies OCIOMYaml::StrategyStringToEnum(const char * enumStr)
{
    auto it = m_mergeStrategiesMap.find(enumStr);
    if (it != m_mergeStrategiesMap.end())
    {
        return it->second;
    }

    return ConfigMergingParameters::MergeStrategies::STRATEGY_UNSPECIFIED;
}

const char * OCIOMYaml::EnumToStrategyString(ConfigMergingParameters::MergeStrategies strategy)
{
    switch (strategy)
    {
        case ConfigMergingParameters::MergeStrategies::STRATEGY_PREFER_INPUT:
            return "PreferInput";
            break;
        case ConfigMergingParameters::MergeStrategies::STRATEGY_PREFER_BASE:
            return "PreferBase";
            break;
        case ConfigMergingParameters::MergeStrategies::STRATEGY_INPUT_ONLY:
            return "InputOnly";
            break;
        case ConfigMergingParameters::MergeStrategies::STRATEGY_BASE_ONLY:
            return "BaseOnly";
            break;
        case ConfigMergingParameters::MergeStrategies::STRATEGY_REMOVE:
            return "Remove";
            break;
        case ConfigMergingParameters::MergeStrategies::STRATEGY_UNSPECIFIED:
            return "Unspecified";
            break;
        default:
            return "Unknown";
            break;
    }
}

namespace
{

inline void save(YamlWrite::Emitter & out, const ConfigMerger & merger)
{
    std::stringstream ss;
    const unsigned parserMajorVersion = merger.getMajorVersion();
    ss << parserMajorVersion;
    ss << "." << merger.getMinorVersion();

    out << YamlWrite::Block;
    out << YamlWrite::BeginMap;
    out << YamlWrite::Key << "ociom_version" << YamlWrite::Value << ss.str();
    out << YamlWrite::Key << "search_path";
    out << YamlWrite::Value << YamlWrite::BeginSeq;
    for (int i = 0; i < merger.getNumSearchPaths(); i++)
    {
        out << YamlWrite::Value << merger.getSearchPath(i);
    }
    out << YamlWrite::EndSeq;
    out << YamlWrite::Newline;

    out << YamlWrite::Key << "merge";
    out << YamlWrite::Value << YamlWrite::BeginMap;

    for (int mp = 0; mp < merger.getNumConfigMergingParameters(); mp++)
    {
        // Serialized every merge section.
        ConfigMergingParametersRcPtr p = merger.getParams(mp);
        out << YamlWrite::Key << p->getOutputName();
        out << YamlWrite::Value << YamlWrite::BeginMap;

        out << YamlWrite::Key << "base" << YamlWrite::Value << p->getBaseConfigName();
        out << YamlWrite::Key << "input" << YamlWrite::Value << p->getInputConfigName();
        out << YamlWrite::Newline;

        out << YamlWrite::Key << "options";
        out << YamlWrite::Value << YamlWrite::BeginMap;
        out << YamlWrite::Key << "input_family_prefix" << YamlWrite::Value << p->getInputFamilyPrefix();
        out << YamlWrite::Key << "base_family_prefix" << YamlWrite::Value << p->getBaseFamilyPrefix();
        out << YamlWrite::Key << "input_first" << YamlWrite::Value << p->isInputFirst();
        out << YamlWrite::Key << "error_on_conflict" << YamlWrite::Value << p->isErrorOnConflict();
        out << YamlWrite::Key << "default_strategy" << YamlWrite::Value << OCIOMYaml::EnumToStrategyString(p->getDefaultStrategy());
        out << YamlWrite::Key << "avoid_duplicates" << YamlWrite::Value << p->isAvoidDuplicates();
        out << YamlWrite::Key << "adjust_input_reference_space" << YamlWrite::Value << p->isAdjustInputReferenceSpace();
        // End of options section.
        out << YamlWrite::EndMap;
        out << YamlWrite::Newline;

        out << YamlWrite::Key << "overrides";
        out << YamlWrite::Value << YamlWrite::BeginMap;
        out << YamlWrite::Key << "name" << YamlWrite::Value << p->getName();
        out << YamlWrite::Key << "description" << YamlWrite::Value << p->getDescription();
        out << YamlWrite::Key << "search_path" << YamlWrite::Value << p->getSearchPath();

        out << YamlWrite::Key << "environment";
        out << YamlWrite::Value << YamlWrite::BeginMap;
        for(int i = 0; i < p->getNumEnvironmentVars(); ++i)
        {   
            const char* name = p->getEnvironmentVar(i);
            out << YamlWrite::Key << name;
            out << YamlWrite::Value << p->getEnvironmentVarValue(i);
        }
        out << YamlWrite::EndMap;
        out << YamlWrite::Newline;

        out << YamlWrite::Key << "active_displays";
        StringUtils::StringVec active_displays;
        if (p->getActiveDisplays() != NULL && strlen(p->getActiveDisplays()) > 0)
            active_displays = SplitStringEnvStyle(p->getActiveDisplays());
        out << YamlWrite::Value << YamlWrite::Flow << active_displays;
        out << YamlWrite::Newline;

        out << YamlWrite::Key << "active_views";
        StringUtils::StringVec active_views;
        if (p->getActiveViews() != NULL && strlen(p->getActiveViews()) > 0)
            active_views = SplitStringEnvStyle(p->getActiveViews());
        out << YamlWrite::Value << YamlWrite::Flow << active_views;

        out << YamlWrite::Key << "inactive_colorspaces";
        StringUtils::StringVec inactive_colorspaces;
        if (p->getInactiveColorSpaces() != NULL && strlen(p->getInactiveColorSpaces()) > 0)
            inactive_colorspaces = SplitStringEnvStyle(p->getInactiveColorSpaces());
        out << YamlWrite::Value << YamlWrite::Flow << inactive_colorspaces;

        // End of overrides section.
        out << YamlWrite::EndMap;
        out << YamlWrite::Newline;

        out << YamlWrite::Key << "params";
        out << YamlWrite::Value << YamlWrite::BeginMap;

        out << YamlWrite::Key << "roles";
        out << YamlWrite::Value << YamlWrite::BeginMap;
        out << YamlWrite::Key << "strategy" << YamlWrite::Value << OCIOMYaml::EnumToStrategyString(p->getRoles());
        out << YamlWrite::EndMap;

        out << YamlWrite::Key << "file_rules";
        out << YamlWrite::Value << YamlWrite::BeginMap;
        out << YamlWrite::Key << "strategy" << YamlWrite::Value << OCIOMYaml::EnumToStrategyString(p->getFileRules());
        out << YamlWrite::EndMap;

        out << YamlWrite::Key << "display-views";
        out << YamlWrite::Value << YamlWrite::BeginMap;
        out << YamlWrite::Key << "strategy" << YamlWrite::Value << OCIOMYaml::EnumToStrategyString(p->getDisplayViews());
        out << YamlWrite::EndMap;

        out << YamlWrite::Key << "view_transforms";
        out << YamlWrite::Value << YamlWrite::BeginMap;
        out << YamlWrite::Key << "strategy" << YamlWrite::Value << OCIOMYaml::EnumToStrategyString(p->getViewTransforms());
        out << YamlWrite::EndMap;

        out << YamlWrite::Key << "looks";
        out << YamlWrite::Value << YamlWrite::BeginMap;
        out << YamlWrite::Key << "strategy" << YamlWrite::Value << OCIOMYaml::EnumToStrategyString(p->getLooks());
        out << YamlWrite::EndMap;

        out << YamlWrite::Key << "colorspaces";
        out << YamlWrite::Value << YamlWrite::BeginMap;
        out << YamlWrite::Key << "strategy" << YamlWrite::Value << OCIOMYaml::EnumToStrategyString(p->getColorspaces());
        out << YamlWrite::EndMap;

        out << YamlWrite::Key << "named_transforms";
        out << YamlWrite::Value << YamlWrite::BeginMap;
        out << YamlWrite::Key << "strategy" << YamlWrite::Value << OCIOMYaml::EnumToStrategyString(p->getNamedTransforms());
        out << YamlWrite::EndMap;

        // End of params section.
        out << YamlWrite::EndMap;

        // End of the current merge section.
        out << YamlWrite::EndMap;    
    }

    // End of the merges.
    out << YamlWrite::EndMap;

    out << YamlWrite::EndMap;
}

} // anon.

void OCIOMYaml::write(std::ostream & ostream, const ConfigMerger & merger)
{
    YamlWrite::Emitter out;
    save(out, merger);
    ostream << out.str();
}

} // namespace OCIO_NAMESPACE
