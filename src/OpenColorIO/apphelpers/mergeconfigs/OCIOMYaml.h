// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the OpenColorIO Project.

#include <unordered_map>
#include <vector>

#include <OpenColorIO/OpenColorIO.h>

#include "YamlNode.h"

#ifndef INCLUDED_OCIOM_YAML_H
#define INCLUDED_OCIOM_YAML_H

namespace OCIO_NAMESPACE
{

// Handles the OCIOM file format parsing.

class OCIOMYaml
{
public:
    OCIOMYaml();
    ~OCIOMYaml() = default;

    void throwValueError(const std::string & nodeName, 
                         const YamlRead::Node & key, 
                         const std::string & msg);
    void CheckDuplicates(const YamlRead::Node & node);
    
    void loadOptions(const YamlRead::Node & node, ConfigMergingParametersRcPtr & params);

    void loadOverrides(const YamlRead::Node & node, ConfigMergingParametersRcPtr & params);

    void loadParams(const YamlRead::Node & node, ConfigMergingParametersRcPtr & params);

    ConfigMergingParameters::MergeStrategies genericStrategyHandler(const YamlRead::Node & pnode, const YamlRead::Node & node);

    void read(std::istream & istream, ConfigMergerRcPtr & merger, const char * filepath);
    void write(std::ostream & ostream, const ConfigMerger & merger);

    void load(const YamlRead::Node & node, std::string & x);
    void load(const YamlRead::Node & node, std::vector<std::string> & x);

    /**
     * \brief Load an OCIOM file.
     */
    void load(const YamlRead::Node& node, ConfigMergerRcPtr & merger, const char * filename);
    
    /**
     * \brief Counts the number of merges in an OCIOM file to calculate the right number of
     *        objects to create.
     */
    int countMerges(const YamlRead::Node& node);

    static ConfigMergingParameters::MergeStrategies StrategyStringToEnum(const char * enumStr);
    static const char * EnumToStrategyString(ConfigMergingParameters::MergeStrategies strategy);

private:
    static std::unordered_map<std::string, ConfigMergingParameters::MergeStrategies> m_mergeStrategiesMap;
};

} // namespace OCIO_NAMESPACE

#endif // INCLUDED_OCIOM_YAML_H
