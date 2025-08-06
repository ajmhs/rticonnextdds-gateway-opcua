/*
 * (c) 2017-2020 Copyright, Real-Time Innovations, Inc. (RTI)
 * All rights reserved.
 *
 * RTI grants Licensee a license to use, modify, compile, and create derivative
 * works of the Software solely in combination with RTI Connext DDS. Licensee
 * may redistribute copies of the Software provided that all such copies are
 * subject to this License. The Software is provided "as is", with no warranty
 * of any type, including any warranty for fitness for any purpose. RTI is
 * under no obligation to maintain or support the Software. RTI shall not be
 * liable for any incidental or consequential damages arising out of the use or
 * inability to use the Software. For purposes of clarity, nothing in this
 * License prevents Licensee from using alternate versions of DDS, provided
 * that Licensee may not combine or link such alternate versions of DDS with
 * the Software.
 */

#include <dds/core/Exception.hpp>
#include <rti/routing/ServiceProperty.hpp>

#include <rti/ddsopcua/DdsOpcUaGatewayException.hpp>

#include "config/XmlSupport.hpp"
#include "service/Utils.hpp"

#include "config/OpcUaBuiltinDataTypes_xml.hpp"
#include "config/rti_dds_opcua_service_xsd.hpp"
#include "config/rti_ddsopcua_to_router_xslt.hpp"

#include "XmlTransformationParams.hpp"
#include "RtiXmlUtilsDecl.hpp"

namespace rti { namespace ddsopcua { namespace config {

STATIC_CONST_STRING_DEFINITION(
        XmlSupport,
        path_to_module_xml,
        "/../resources/xml/");

STATIC_CONST_STRING_DEFINITION(
        XmlSupport,
        path_to_shipped_xml,
        "/../../../xml/");

STATIC_CONST_STRING_DEFINITION(
        XmlSupport,
        default_configuration_file_name,
        "RTI_DDS_OPCUA_SERVICE.xml");

STATIC_CONST_STRING_DEFINITION(
        XmlSupport,
        user_configuration_file_name,
        "USER_RTI_DDS_OPCUA_SERVICE.xml");

STATIC_CONST_STRING_DEFINITION(XmlSupport, service_tag, "ddsopcua_service");
STATIC_CONST_STRING_DEFINITION(
        XmlSupport,
        opcua2ddsbridge_tag,
        "opcua_to_dds_bridge");
STATIC_CONST_STRING_DEFINITION(XmlSupport, include_tag, "include");

STATIC_CONST_STRING_DEFINITION(XmlSupport, service_variables_tag, "configuration_variables");
STATIC_CONST_STRING_DEFINITION(XmlSupport, variables_element_tag, "element");
STATIC_CONST_STRING_DEFINITION(XmlSupport, bridge_publication_tag, "publication");
STATIC_CONST_STRING_DEFINITION(XmlSupport, bridge_subscription_tag, "subscription");


XmlSupport::XmlSupport(
        const rti::ddsopcua::GatewayProperty& properties,
        bool validate_on_parse)
        : xml_root_(nullptr),
          validate_on_parse_(validate_on_parse),
          validator_(nullptr, RTIXMLUTILSValidator_delete),
          transformer_(nullptr, RTIXMLUTILSTransformer_delete),
          user_env_(properties.user_environment()),
          verbosity_(properties.verbosity())
{
    // Register included schemas
    RTIXMLUTILSValidator *validator = RTIXMLUTILSValidator_newFromStringArray(
            DDSOPCUA_XSD,
            DDSOPCUA_XSD_SIZE);
    if (validator == nullptr) {
        RTI_THROW_GATEWAY_EXCEPTION(
                &DDSOPCUA_LOG_CREATION_FAILURE_s,
                "XSD validator");
    }
    validator_.reset(validator);

    // Register XSLT transformation
    RTIXMLUTILSTransformer *trfmr = RTIXMLUTILSTransformer_newFromStringArray(
            DDSOPCUA_XSLT,
            DDSOPCUA_XSLT_SIZE);
    if (trfmr == nullptr) {
        RTI_THROW_GATEWAY_EXCEPTION(
                &DDSOPCUA_LOG_CREATION_FAILURE_s,
                "XSLT transformation");
    }
    transformer_.reset(trfmr);
}

XmlSupport::~XmlSupport()
{
    if (xml_root_ != nullptr) {
        RTIXMLUTILSParser_freeDom(xml_root_);
        xml_root_ = nullptr;
    }
}

void XmlSupport::finalize_globals()
{
    RTIXMLUTILSGlobals_finalize();
}

void XmlSupport::initialize_globals()
{ }

void XmlSupport::parse_file(const std::string& filename)
{
    std::string normalized_filename = utils::normalize_path(filename);

    GATEWAYLog_warn(
            &DDSOPCUA_LOG_PARSER_LOAD_FILE_s,
            normalized_filename.c_str());

    struct RTIXMLUTILSObject *xml_object = nullptr;
    if (!RTIXMLUTILSParser_parseUrlGroupList(&xml_object, filename.c_str())) {
        RTI_THROW_GATEWAY_EXCEPTION(
                &DDSOPCUA_LOG_PARSER_PARSE_FILE_FAILURE_s,
                normalized_filename.c_str());
    }

    process_loaded_xml(xml_object);

    // Manage any include definitions in the xml
    parse_include_files(filename);

    // Manage any internal variables used in the xml
    resolve_variables();
}

void XmlSupport::parse_string_array(
        const char **string_array,
        int string_array_length)
{
    struct RTIXMLUTILSObject *xml_object = nullptr;
    if (!RTIXMLUTILSParser_parseStringArray(
                &xml_object,
                string_array,
                string_array_length)) {
        RTI_THROW_GATEWAY_EXCEPTION(
                &DDSOPCUA_LOG_PARSER_PARSE_FAILURE_s,
                "OPC UA built-in types XML definition");
    }

    process_loaded_xml(xml_object);
}

void XmlSupport::parse_types_string_array(
        const char **string_array,
        int string_array_length)
{
    struct RTIXMLUTILSObject *dds_xml_object = nullptr;
    if (!RTIXMLUTILSParser_parseString(&dds_xml_object, "<dds/>")) {
        RTI_THROW_GATEWAY_EXCEPTION(
                &DDSOPCUA_LOG_PARSER_PARSE_FAILURE_s,
                "DDS Empty XML definition");
    }

    struct RTIXMLUTILSObject *types_xml_object = nullptr;
    if (!RTIXMLUTILSParser_parseStringArray(
                &types_xml_object,
                string_array,
                string_array_length)) {
        RTIXMLUTILSParser_freeDom(dds_xml_object);
        RTI_THROW_GATEWAY_EXCEPTION(
                &DDSOPCUA_LOG_PARSER_PARSE_FAILURE_s,
                "OPC UA built-in types XML definition");
    }

    if (RTIXMLUTILSObject_copyAsChild(dds_xml_object, types_xml_object)
        == nullptr) {
        RTIXMLUTILSParser_freeDom(dds_xml_object);
        RTIXMLUTILSParser_freeDom(types_xml_object);
        RTI_THROW_GATEWAY_EXCEPTION(
                &DDSOPCUA_LOG_PARSER_PARSE_FAILURE_s,
                "Merge OPC UA built-in types and DDS XML definition");
    }

    RTIXMLUTILSParser_freeDom(types_xml_object);

    process_loaded_xml(dds_xml_object);
}

static void process_include(
        RTIXMLUTILSObject *inc_ele,
        const std::string& source_file,
        RTIXMLUTILSObject *merge_target)
{
    bool error = false;

    const char *file_name =
            RTIXMLUTILSObject_getAttribute(inc_ele, "file");

    if (file_name == nullptr) {
        RTI_THROW_GATEWAY_EXCEPTION(
                &DDSOPCUA_LOG_PARSER_PARSE_FAILURE_s,
                "Include element without 'file' attribute");
    }

    // if the include element has a base attribute, use it
    const char *base = RTIXMLUTILSObject_getAttribute(inc_ele, "base");

    std::string normalized_file_name;
    if (base == nullptr || (base != nullptr && strcmp(base, "relative") == 0)) {
        // Relative path, prepend the file path from the XML root
        normalized_file_name = utils::normalize_path(
                utils::dirname(source_file) + '/' + file_name);
    } else if (strcmp(base, "root") == 0) {
        // Root path, prepend the executable path
        normalized_file_name = utils::normalize_path(
                utils::executable_path() + '/' + file_name);
    } else if (strcmp(base, "absolute") == 0) {
        // Absolute path, use as is
        normalized_file_name = utils::normalize_path(file_name);
    }

    GATEWAYLog_warn(
            &DDSOPCUA_LOG_PARSER_LOAD_FILE_s,
            normalized_file_name.c_str());

    struct RTIXMLUTILSObject *incl_root = nullptr;
    if (!RTIXMLUTILSParser_parseUrlGroupList(
                &incl_root,
                normalized_file_name.c_str())) {
        const char *on_missing =
                RTIXMLUTILSObject_getAttribute(inc_ele, "onMissing");

        if (on_missing == nullptr
            || (on_missing != nullptr && strcmp(on_missing, "log") == 0)) {
            GATEWAYLog_warn(
                    &DDSOPCUA_LOG_PARSER_PARSE_FILE_FAILURE_s,
                    normalized_file_name.c_str());

        } else if (strcmp(on_missing, "error") == 0) {
            RTI_THROW_GATEWAY_EXCEPTION(
                    &DDSOPCUA_LOG_PARSER_PARSE_FILE_FAILURE_s,
                    normalized_file_name.c_str());
        }

        return;  // Skip this include if it fails and the defined action is not
                 // to throw
    }

    // Iterate over all the children of the include element and copy them to the merge target
    for (auto *incl_child = RTIXMLUTILSObject_getFirstChild(incl_root);
         incl_child;
         incl_child = RTIXMLUTILSObject_getNextSibling(incl_child)) {
        if (!RTIXMLUTILSObject_copyAsChild(merge_target, incl_child)) {
            error = true;
            break;
        }
    }

    // Free the included XML object
    RTIXMLUTILSParser_freeDom(incl_root);

    // If there was an error, throw an exception
    if (error) {
        RTI_THROW_GATEWAY_EXCEPTION(
                &DDSOPCUA_LOG_ANY_ss,
                "Error merging to target node with included file ",
                normalized_file_name.c_str());
    }
}

void XmlSupport::parse_include_files(const std::string& source_file)
{
    // Parse <include> elements at the global scope
    for (auto *inc_ele = RTIXMLUTILSObject_getFirstChildWithTag(
                 xml_root_,
                 include_tag().c_str());
         inc_ele;
         inc_ele = RTIXMLUTILSObject_getNextSiblingWithTag(
                 inc_ele,
                 include_tag().c_str())) {
        process_include(inc_ele, source_file, xml_root_);
    }

    // Parse includes at the service scope
    parse_service_includes(source_file);
}

void XmlSupport::parse_service_includes(const std::string& source_file)
{
    // Parse <include> elements at the service scope
    for (auto *service = RTIXMLUTILSObject_getFirstChildWithTag(
                 xml_root_,
                 service_tag().c_str());
         service;
         service = RTIXMLUTILSObject_getNextSiblingWithTag(
                 service,
                 service_tag().c_str())) {

        // Parse <include> elements within the service element
        for (auto *inc_ele = RTIXMLUTILSObject_getFirstChildWithTag(
                     service,
                     include_tag().c_str());
             inc_ele;
             inc_ele = RTIXMLUTILSObject_getNextSiblingWithTag(
                     inc_ele,
                     include_tag().c_str())) {

            process_include(inc_ele, source_file, service);
        }
        // Process bridge includes for this service
        parse_bridge_includes(service, source_file);
    }
}

void XmlSupport::parse_bridge_includes(
        RTIXMLUTILSObject *service_object,
        const std::string& source_file)
{
    // Parse <include> elements at the bridge scope
    for (auto *bridge_element = RTIXMLUTILSObject_getFirstChildWithTag(
                 service_object,
                 opcua2ddsbridge_tag().c_str());
         bridge_element;
         bridge_element = RTIXMLUTILSObject_getNextSiblingWithTag(
                 bridge_element,
                 opcua2ddsbridge_tag().c_str())) {

        // For each include element in the bridge
        for (auto *inc_ele = RTIXMLUTILSObject_getFirstChildWithTag(
                     bridge_element,
                     include_tag().c_str());
             inc_ele != nullptr;
             inc_ele = RTIXMLUTILSObject_getNextSiblingWithTag(
                     inc_ele,
                     include_tag().c_str())) {

            process_include(inc_ele, source_file, bridge_element);
        }
    }
}

using stringmap_t = std::map<std::string, std::string>;

static void parse_variables(RTIXMLUTILSObject *element, stringmap_t& variables)
{
    // Find the configuration_variables element, if it exists
    RTIXMLUTILSObject *config_vars_element =
            RTIXMLUTILSObject_getFirstChildWithTag(
                    element,
                    XmlSupport::service_variables_tag().c_str());

    if (config_vars_element != nullptr) {
        for (auto *var_ele =
                     RTIXMLUTILSObject_getFirstChild(config_vars_element);
             var_ele != nullptr;
             var_ele = RTIXMLUTILSObject_getNextSiblingWithTag(
                     var_ele,
                     XmlSupport::variables_element_tag().c_str())) {
                        
            RTIXMLUTILSObject *var_name =
                    RTIXMLUTILSObject_getFirstChildWithTag(var_ele, "name");
            RTIXMLUTILSObject *var_value =
                    RTIXMLUTILSObject_getFirstChildWithTag(var_ele, "value");

            if (var_name != nullptr && var_value != nullptr) {
                const char *name = RTIXMLUTILSObject_getText(var_name);
                const char *value = RTIXMLUTILSObject_getText(var_value);

                if (name != nullptr && value != nullptr) {
                    variables[name] = value;
                }
            }
        }
    }
}

static bool substitute_variables(
        const std::string& text,
        const stringmap_t& variables,
        std::string& result)
{
    
    bool changed = false;
    result = text;
    for (const auto& var : variables) {
        std::string placeholder = "${" + var.first + "}";
        size_t pos = result.find(placeholder);
        while (pos != std::string::npos) {
            result.replace(pos, placeholder.length(), var.second);
            changed = true;

            pos = result.find(placeholder, pos + var.second.length());            
        }
    }
    return changed;
}

static void process_target_variables(
        RTIXMLUTILSObject *target,
        const stringmap_t& variables)
{
    std::string substituted_text;
    if (RTIXMLUTILSObject_isText(target)) {
        const char *text = RTIXMLUTILSObject_getText(target);
        if (text && substitute_variables(text, variables, substituted_text)) {
            RTIXMLUTILSObject_setText(target, substituted_text.c_str());
        }
    }
 
    // Recursively process child elements
    for (auto* child = RTIXMLUTILSObject_getFirstChild(target); 
         child; child = RTIXMLUTILSObject_getNextSibling(child)) {
        process_target_variables(child, variables);
    }
}

static void resolve_target_variables(RTIXMLUTILSObject *target)
{
    // Get the variables from the target element
    stringmap_t variables;
    parse_variables(target, variables);
    if (!variables.empty()) {

        // Process all child elements of target except the variables element
        for (auto *child = RTIXMLUTILSObject_getFirstChild(target); child;
             child = RTIXMLUTILSObject_getNextSibling(child)) {
            const char *tag_name = RTIXMLUTILSObject_getTagName(child);
            if (tag_name == nullptr
                || strcmp(tag_name, "configuration_variables") != 0) {
                process_target_variables(child, variables);
            }
        }
    }
}

void XmlSupport::resolve_variables()
{
    // For each service element
    for (auto *service = RTIXMLUTILSObject_getFirstChildWithTag(
                 xml_root_,
                 service_tag().c_str());
         service;
         service = RTIXMLUTILSObject_getNextSiblingWithTag(
                 service,
                 service_tag().c_str())) {

        // For each bridge element in the service
        for (auto *bridge = RTIXMLUTILSObject_getFirstChildWithTag(
                     service,
                     opcua2ddsbridge_tag().c_str());
             bridge;
             bridge = RTIXMLUTILSObject_getNextSiblingWithTag(
                     bridge,
                     opcua2ddsbridge_tag().c_str())) {
            
            // For each publication element in the bridge
            for (auto *pub = RTIXMLUTILSObject_getFirstChildWithTag(
                         bridge,
                         bridge_publication_tag().c_str());
                 pub;
                 pub = RTIXMLUTILSObject_getNextSiblingWithTag(
                         pub,
                         bridge_publication_tag().c_str())) {

                // Resolve variables in the publication element
                resolve_target_variables(pub);
            }

            // For each subscription element in the bridge
            for (auto *sub = RTIXMLUTILSObject_getFirstChildWithTag(
                         bridge,
                         bridge_subscription_tag().c_str());
                 sub;
                 sub = RTIXMLUTILSObject_getNextSiblingWithTag(
                         sub,
                         bridge_subscription_tag().c_str())) {

                // Resolve variables in the subscription element
                resolve_target_variables(sub);
            }
        }
    }
}

void XmlSupport::process_loaded_xml(RTIXMLUTILSObject *xml_object)
{
    bool error = false;
    const RTILogMessage *error_template = nullptr;
    const char *error_msg = nullptr;
    bool free_dom = true;
    /* replace variables */
    struct RTIXMLUTILSPropertyList dictionary = { nullptr, 0, 0 };
    RTI_RoutingServiceProperties native_properties;
    RTI_RoutingServiceProperties_initialize(&native_properties);
    rti::routing::PropertyAdapter::add_properties_to_native(
            &native_properties,
            user_env_);
    dictionary._propertyArray = reinterpret_cast<RTIXMLUTILSProperty *>(
            native_properties.properties);
    dictionary._propertyArrayLength = native_properties.count;
    if (!RTIXMLUTILSVariableExpansor_expandFromEnvironmentOrDictionary(
                xml_object,
                &dictionary)) {
        error = true;
        error_template = &DDSOPCUA_LOG_ANY_FAILURE_s;
        error_msg = "replace variables in configuration";
    }
    RTI_RoutingServiceProperties_finalize(&native_properties);
    /* validate */
    if (!error && validate_on_parse_
        && !RTIXMLUTILSValidator_validate(validator_.get(), xml_object)) {
        error = true;
        error_template = &DDSOPCUA_LOG_VALIDATOR_VALIDATE_FILE_FAILURE_s;
        error_msg = RTIXMLUTILSObject_getFilePath(xml_object);
    }
    /* merge */
    if (!error && xml_root_ == nullptr) {
        xml_root_ = xml_object;
        free_dom = false;
    } else if (!error && !RTIXMLUTILSParser_mergeRoot(xml_root_, xml_object)) {
        error = true;
        error_template = &DDSOPCUA_LOG_PARSER_PARSE_FAILURE_s;
        error_msg = RTIXMLUTILSObject_getFilePath(xml_object);
    }

    // Make sure we always free the DOM
    if (free_dom) {
        RTIXMLUTILSParser_freeDom(xml_object);
    }
    if (error) {
        RTI_THROW_GATEWAY_EXCEPTION(error_template, error_msg);
    }
}

void XmlSupport::convert_xml_to_configuration_strings(
        std::vector<std::string>& cfg_strings,
        RTIXMLUTILSObject *xml_object)
{
    std::unique_ptr<char, void (*)(char *)> string_configuration(
            RTIXMLUTILSObject_toString(xml_object),
            DDS_String_free);
    cfg_strings.resize(1);
    cfg_strings[0] = string_configuration.get();
}

XmlSupport::native_xmlutilsobject XmlSupport::to_router_configuration()
{
    RTIXMLUTILSObject *transformed = RTIXMLUTILSTransformer_transformWithParams(
            transformer_.get(),
            xml_root_,
            &XmlTransformationParams::transformation_params()[0],
            static_cast<int>(
                    XmlTransformationParams::transformation_params().size()));
    if (transformed == nullptr) {
        RTI_THROW_GATEWAY_EXCEPTION(
                &DDSOPCUA_LOG_TRANSFORMER_TRANSFORM_FAILURE_ss,
                "OPC UA/DDS Gateway",
                "Router");
    }

    // check the gateway verbosity level to decide whether to dump the
    // transformed XML
    if (verbosity_ >= rti::config::Verbosity::WARNING) {
        printf("%s\n", RTIXMLUTILSObject_toString(transformed));
    }

    return XmlSupport::native_xmlutilsobject(
            transformed,
            RTIXMLUTILSParser_freeDom);
}

bool XmlSupport::load_default_files()
{
    // Load built-in types
    parse_types_string_array(
            OPCUA_BUILTIN_TYPES_XML,
            OPCUA_BUILTIN_TYPES_XML_SIZE);

    char executable_path[DDSOPCUA_FILE_PATH_MAX_LENGTH + 1] = { 0 };
    RTIOsapiUtility_getSelfDirectoryPath(
            executable_path,
            DDSOPCUA_FILE_PATH_MAX_LENGTH);
    // Try to load the default files from the shipped location
    std::string cfg_file;
    cfg_file += executable_path;
    cfg_file += XmlSupport::path_to_shipped_xml();
    cfg_file += default_configuration_file_name();

    std::string normalized_path = utils::normalize_path(cfg_file).c_str();
    if (utils::file_exists(normalized_path)) {
        parse_file(normalized_path);
    } else {
        // If files aren't in the default location, try to load the default
        // files from the shipped location
        cfg_file.clear();
        cfg_file += executable_path;
        cfg_file += XmlSupport::path_to_module_xml();
        cfg_file += default_configuration_file_name();

        normalized_path = utils::normalize_path(cfg_file).c_str();
        if (utils::file_exists(normalized_path)) {
            parse_file(normalized_path);
        } else {
            GATEWAYLog_warn(
                    &RTI_OSAPI_UTILITY_LOG_FILE_NOT_FOUND_s,
                    normalized_path.c_str());
        }
    }

    // Load the user file name (from working directory only)
    if (utils::file_exists(user_configuration_file_name())) {
        parse_file(user_configuration_file_name());
    } else {
        GATEWAYLog_warn(
                &RTI_OSAPI_UTILITY_LOG_FILE_NOT_FOUND_s,
                user_configuration_file_name().c_str());
    }

    return true;
}

void XmlSupport::print_available_configurations()
{
    struct RTIXMLUTILSObject *xml_service = nullptr;
    int configurations_count = 0;

    printf("Available configurations:\n");

    xml_service = RTIXMLUTILSObject_getFirstChildWithTag(
            xml_root_,
            this->service_tag().c_str());

    while (xml_service != nullptr) {
        const char *annotation_doc = nullptr;
        annotation_doc = RTIXMLUTILSObject_getAnnotationDocText(xml_service);

        if (*annotation_doc == '\0') {
            printf("- %s (%s)\n",
                   RTIXMLUTILSObject_getName(xml_service),
                   RTIXMLUTILSObject_getFilePath(xml_service));
        } else {
            printf("- %s:(%s)\n\t%s\n",
                   RTIXMLUTILSObject_getName(xml_service),
                   RTIXMLUTILSObject_getFilePath(xml_service),
                   annotation_doc);
        }

        xml_service = RTIXMLUTILSObject_getNextSiblingWithTag(
                xml_service,
                RTIXMLUTILSObject_getTagName(xml_service));
        configurations_count++;
    }

    if (configurations_count == 0) {
        printf("No configurations found\n");
    }
}

RTIXMLUTILSObject *XmlSupport::xml_root() const
{
    return xml_root_;
}

void XmlSupport::validate(RTIXMLUTILSObject *xml_object)
{
    if (!RTIXMLUTILSValidator_validate(validator_.get(), xml_object)) {
        RTI_THROW_GATEWAY_EXCEPTION(
                &DDSOPCUA_LOG_VALIDATOR_VALIDATE_ENTITY_FAILURE_s,
                RTIXMLUTILSObject_getTagName(xml_object));
    }
}

}}}  // namespace rti::ddsopcua::config
