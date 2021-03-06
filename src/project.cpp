#include "project.h"
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include "wrapper/filesystem.h"
#include "util.h"
#include "rompatcher.h"
#include "exceptions.h"

namespace sable {

Project::Project(const YAML::Node &config, const std::string &projectDir) : nextAddress(0)
{
    init(config, projectDir);
}

Project::Project(const std::string &projectDir) : nextAddress(0)
{
    if (!fs::exists(fs::path(projectDir) / "config.yml")) {
        throw ConfigError((fs::path(projectDir) / "config.yml").string() + " not found.");
    }
    YAML::Node config = YAML::LoadFile((fs::path(projectDir) / "config.yml").string());
    init(config, projectDir);
}

void Project::init(const YAML::Node &config, const std::string &projectDir)
{
    using std::string;
    using std::vector;
    if (validateConfig(config)) {
        YAML::Node outputConfig = config[FILES_SECTION][OUTPUT_SECTION];
        m_MainDir = config[FILES_SECTION][DIR_MAIN].as<std::string>();
        m_InputDir = config[FILES_SECTION][INPUT_SECTION][DIR_VAL].as<string>();
        m_OutputDir = outputConfig[DIR_VAL].as<string>();
        m_BinsDir = outputConfig[OUTPUT_BIN][DIR_MAIN].as<string>();
        m_TextOutDir = outputConfig[OUTPUT_BIN][DIR_TEXT].as<string>();
        m_FontDir = outputConfig[OUTPUT_BIN][FONT_SECTION][DIR_FONT].as<string>();
        m_RomsDir = config[FILES_SECTION][DIR_ROM].as<string>();
        fs::path mainDir(projectDir);
        if (fs::path(m_MainDir).is_relative()) {
            m_MainDir = (mainDir / m_MainDir).string();
        }
        if (fs::path(m_RomsDir).is_relative()) {
            m_RomsDir = (mainDir / m_RomsDir).string();
        }
        if (outputConfig[OUTPUT_BIN][FONT_SECTION][INCLUDE_VAL].IsSequence()) {
            m_FontIncludes = outputConfig[OUTPUT_BIN][FONT_SECTION][INCLUDE_VAL].as<vector<string>>();
        }
        if (outputConfig[OUTPUT_BIN][EXTRAS].IsSequence()) {
            m_Extras = outputConfig[OUTPUT_BIN][EXTRAS].as<vector<string>>();
        }
        if (outputConfig[INCLUDE_VAL].IsSequence()){
            m_Includes = outputConfig[INCLUDE_VAL].as<vector<string>>();
        }
        m_Roms = config[ROMS].as<vector<Rom>>();
        fs::path fontLocation = mainDir
                / config[CONFIG_SECTION][DIR_VAL].as<string>()
                / config[CONFIG_SECTION][IN_MAP].as<string>();
        std::string defaultMode = config[CONFIG_SECTION][DEFAULT_MODE].IsDefined()
                ? config[CONFIG_SECTION][DEFAULT_MODE].as<string>() : "normal";
        if (!fs::exists(fontLocation)) {
            throw ConfigError(fontLocation.string() + " not found.");
        }
        m_Parser = TextParser(YAML::LoadFile(fontLocation.string()), defaultMode);
    }
}

bool Project::parseText()
{
    fs::path mainDir(m_MainDir);
    {
        if (!fs::exists(mainDir / m_OutputDir / m_BinsDir)) {
            if (!fs::exists(mainDir / m_OutputDir)) {
                fs::create_directory(mainDir / m_OutputDir);
            }
            if (!fs::exists(mainDir / m_OutputDir / m_BinsDir)) {
                fs::create_directory(mainDir / m_OutputDir / m_BinsDir);
            }
        } else {
            fs::remove_all(mainDir / m_OutputDir / m_BinsDir / m_TextOutDir);
        }
        fs::create_directory(mainDir / m_OutputDir / m_BinsDir / m_TextOutDir);
    }

    {
        fs::path input = fs::path(m_MainDir) / m_InputDir;
        std::vector<std::string> allFiles;
        std::vector<fs::path> dirs;
        std::copy(fs::directory_iterator(input), fs::directory_iterator(), std::back_inserter(dirs));
        std::sort(dirs.begin(), dirs.end());
        for (auto& dir: dirs) {
            if (fs::is_directory(dir)) {
                std::vector<std::string> files;
                if (fs::exists(dir / "table.txt")) {
                    std::string path = (dir / "table.txt").string();
                    std::ifstream tablefile(path);
                    fs::path dir(fs::path(path).parent_path());
                    std::string label = dir.filename().string();
                    Table table;
                    table.setAddress(nextAddress);
                    try {
                        files = table.getDataFromFile(tablefile);
                        for (std::string& file: files) {
                            if (!fs::exists(dir / file)) {
                                std::ostringstream error;
                                error << "In " + path
                                             + ": file " + fs::absolute(dir / file).string()
                                             + " does not exist.";
                                m_Warnings.push_back(error.str());
                            }
                            file = (dir / file).string();
                        }
                    } catch (std::runtime_error &e) {
                        throw ParseError("Error in table " + path + ", " + e.what());
                    }

                    tablefile.close();
                    m_TableList[label] = table;
                    m_Addresses.push_back({table.getAddress(), label, true});
                } else {
                    for (auto& fileIt: fs::directory_iterator(dir)) {
                        if(fs::is_regular_file(fileIt.path())) {
                            files.push_back(fileIt.path().string());
                        }
                    }
                }
                std::sort(files.begin(), files.end());
                allFiles.insert(allFiles.end(), files.begin(), files.end());
            }
        }
        std::string dir = "";
        int dirIndex;
        for (auto &file: allFiles) {
            if (dir != fs::path(file).parent_path().filename().string()) {
                dir = fs::path(file).parent_path().filename().string();
                nextAddress = m_TableList[dir].getDataAddress();
                dirIndex = 0;
            }
            std::ifstream input(file);
            std::vector<unsigned char> data;
            ParseSettings settings =  m_Parser.getDefaultSetting(nextAddress);
            int line = 0;
            while (input) {
                bool done;
                int length;
                try {
                    std::tie(done, length) = m_Parser.parseLine(input, settings, std::back_inserter(data));
                    line++;
                } catch (std::runtime_error &e) {
                    throw ParseError("Error in text file " + file + ": " + e.what());
                }
                if (settings.maxWidth > 0 && length > settings.maxWidth) {
                    std::ostringstream error;
                    error << "In " + file + ": line " + std::to_string(line)
                                 + " is longer than the specified max width of "
                                 + std::to_string(settings.maxWidth) + " pixels.";
                    m_Warnings.push_back(error.str());
                }
                if (done && !data.empty()) {
                    if (m_Parser.getFonts().at(settings.mode)) {
                        if (settings.label.empty()) {
                            std::ostringstream lstream;
                            lstream << dir << '_' << dirIndex++;
                            settings.label = lstream.str();
                        }
                        m_Addresses.push_back({settings.currentAddress, settings.label, false});
                        {
                            int tmpAddress = settings.currentAddress + data.size();
                            size_t dataLength;
                            fs::path binFileName = mainDir / m_OutputDir / m_BinsDir / m_TextOutDir / (settings.label + ".bin");
                            bool printpc = settings.printpc;
                            if ((tmpAddress & 0xFF0000) != (settings.currentAddress & 0xFF0000)) {
                                size_t bankLength = ((settings.currentAddress + data.size()) & 0xFFFF);
                                dataLength = data.size() - bankLength;
                                fs::path bankFileName = binFileName.parent_path() / (settings.label + "bank.bin");
                                outputFile(bankFileName.string(), data, bankLength, dataLength);
                                settings.currentAddress = util::PCToLoROM(util::LoROMToPC(settings.currentAddress | 0xFFFF) +1);
                                m_Addresses.push_back({settings.currentAddress, "$" + settings.label, false});
                                settings.currentAddress += bankLength;
                                m_TextNodeList["$" + settings.label] = {
                                    bankFileName.filename().string(),
                                    bankLength,
                                    printpc
                                };
                                printpc = false;
                            } else {
                                dataLength = data.size();
                                settings.currentAddress += data.size();
                            }
                            outputFile(binFileName.string(), data, dataLength);
                            m_TextNodeList[settings.label] = {
                                binFileName.filename().string(), dataLength, printpc
                            };
                        }
                        settings.label = "";
                        settings.printpc = false;
                        data.clear();
                    }
                }
            }
            nextAddress = settings.currentAddress;
            if (settings.maxWidth < 0) {
                settings.maxWidth = 0;
            }
        }
    }

    {
        std::ofstream mainText((mainDir / m_OutputDir / "text.asm").string());
        std::ofstream textDefines((mainDir / m_OutputDir / "textDefines.exp").string());
        if (!mainText || !textDefines) {
            throw ASMError("Could not open files in " + (mainDir / m_OutputDir).string() + " for writing.\n");
        }

        std::sort(m_Addresses.begin(), m_Addresses.end(), [](AddressNode a, AddressNode b) {
            return b.address > a.address;
        });
        //int lastPosition = 0;
        for (auto& it : m_Addresses) {
            //int dif = it.address - lastPosition;
            if (it.isTable) {
                textDefines << "!def_table_" + it.label + " = $" << std::hex << it.address << '\n';
                mainText  << "ORG !def_table_" + it.label + '\n';
                Table t = m_TableList.at(it.label);
                mainText << "table_" + it.label + ":\n";
                for (auto it : t) {
                    int size;
                    std::string dataType;
                    if (t.getAddressSize() == 3) {
                        dataType = "dl";
                    } else if(t.getAddressSize() == 2) {
                        dataType = "dw";
                    } else {
                        throw std::logic_error("Unsupported address size " + std::to_string(t.getAddressSize()));
                    }
                    if (it.address > 0) {
                        mainText << dataType + " $" << std::setw(4) << std::setfill('0') << std::hex << it.address;
                        size = it.size;
                    } else {
                        mainText << dataType + ' ' + it.label;
                        size = m_TextNodeList.at(it.label).size;
                    }
                    if (t.getStoreWidths()) {
                        mainText << ", " << std::dec << size;
                    }
                    mainText << '\n';
                }
            } else {
                if (it.label.front() == '$') {
                    mainText  << "ORG $" << std::hex << it.address << '\n';
                } else {
                    textDefines << "!def_" + it.label + " = $" << std::hex << it.address << '\n';
                    mainText  << "ORG !def_" + it.label + '\n'
                              << it.label + ":\n";

                }
                std::string token = m_TextNodeList.at(it.label).files;
                mainText << "incbin " + (fs::path(m_BinsDir) / m_TextOutDir / token).string() + '\n';
                if (m_TextNodeList.at(it.label).printpc) {
                    mainText << "print pc\n";
                }
            }
            mainText << '\n';
        }
        textDefines.flush();
        textDefines.close();
        mainText.flush();
        mainText.close();
        fs::path mainDir(m_MainDir);
        for (Rom& romData: m_Roms) {
            std::string patchFile = (mainDir / (romData.name + ".asm")).string();
            std::ofstream mainFile(patchFile);
            mainFile << "lorom\n\n";
            if (!romData.includes.empty()) {
                for (std::string& include: romData.includes) {
                    mainFile << "incsrc " + m_OutputDir + '/' + include << '\n';
                }
            }
            if (!m_Includes.empty()) {
                for (std::string& include: m_Includes) {
                    mainFile << "incsrc " + m_OutputDir + '/' + include << '\n';
                }
            }
            if (!m_Extras.empty()) {
                for (std::string& include: m_Extras) {
                    mainFile << "incsrc " + m_OutputDir + '/'
                                + m_BinsDir + '/' + include + '/' + include + ".asm"
                             << '\n';
                }
            }
            if (!m_FontDir.empty()) {
                mainFile << "incsrc " + m_OutputDir + '/'
                            + m_BinsDir + '/' + m_FontDir + '/' + m_FontDir + ".asm"
                         << '\n';
            }
            mainFile <<  "incsrc " + m_OutputDir + "/textDefines.exp\n"
                        +  "incsrc " + m_OutputDir + "/text.asm\n";
            mainFile.close();
        }
        writeFontData();
    }
    return true;
}

void Project::writePatchData()
{
    fs::path mainDir(m_MainDir);
    for (Rom& romData: m_Roms) {
        std::string patchFile = (mainDir / (romData.name + ".asm")).string();

        fs::path romFilePath = fs::path(m_RomsDir) / romData.file;
        std::string extension = romFilePath.extension().string();
        RomPatcher r(
                     romFilePath.string(),
                    romData.name,
                    "lorom",
                    romData.hasHeader
                    );
        r.expand(util::LoROMToPC(getMaxAddress()));
        auto result = r.applyPatchFile(patchFile);
        if (result) {
            std::cout << "Assembly for " << romData.name << " completed successfully." << std::endl;
        }
        std::vector<std::string> messages;
        r.getMessages(std::back_inserter(messages));
        if (result) {
            for (auto& msg: messages) {
                std::cout << msg << std::endl;
            }
            std::ofstream output(
                        (fs::path(m_RomsDir) / (romData.name + extension)).string(),
                        std::ios::out|std::ios::binary
                        );
            output.write((char*)&r.at(0), r.getRealSize());
            output.close();
        } else {
            for (auto& msg: messages) {
                std::ostringstream error;
                error << msg << '\n';
                throw ASMError(error.str());
            }
        }

    }
}

void Project::writeFontData()
{
    fs::path fontFilePath = fs::path(m_MainDir) / m_OutputDir / m_BinsDir / m_FontDir / (m_FontDir + ".asm");
    std::ofstream output(fontFilePath.string());
    if (!output) {
        throw ASMError("Could not open " + fontFilePath.string() + "for writing.\n");
    }
    for (std::string& include: m_FontIncludes) {
        output << "incsrc " + include + ".asm\n";
    }
    for (auto& fontIt: m_Parser.getFonts()) {
        if (!fontIt.second.getFontWidthLocation().empty()) {
            output << "\n"
                      "ORG " + fontIt.second.getFontWidthLocation();
            std::vector<int> widths;
            widths.reserve(fontIt.second.getMaxEncodedValue());
            fontIt.second.getFontWidths(std::back_insert_iterator(widths));
            int column = 0;
            int skipCount = 0;
            for (auto it = widths.begin(); it != widths.end(); ++it) {
                int width = *it;
                if (width == 0) {
                    skipCount++;
                    column = 0;
                } else {
                    if (skipCount > 0) {
                        output << "\n"
                                  "skip " << std::dec << skipCount;
                        skipCount = 0;
                    }
                    if (column == 0) {
                        output << '\n' << "db ";
                    } else {
                        output << ", ";
                    }
                    output << "$" << std::hex << std::setw(2) << std::setfill('0') << width;
                    column++;
                    if (column ==16) {
                        column = 0;
                    }
                }
            }
            output << '\n' ;
        }
    }
    output.close();
}

std::string Project::MainDir() const
{
    return m_MainDir;
}

std::string Project::RomsDir() const
{
    return fs::absolute(m_RomsDir).string();
}

std::string Project::FontConfig() const
{
    return fs::absolute(fs::path(m_MainDir) / m_OutputDir / m_BinsDir / m_FontDir).string();
}

std::string Project::TextOutDir() const
{
    return fs::absolute(fs::path(m_MainDir) / m_OutputDir / m_BinsDir / m_TextOutDir).string();
}

int Project::getMaxAddress() const
{
    if (m_Addresses.empty()) {
        return 0;
    }
    return m_Addresses.back().address;
}

int Project::getWarningCount() const
{
    return m_Warnings.size();
}

StringVector::const_iterator Project::getWarnings() const
{
    return m_Warnings.begin();
}

sable::Project::operator bool() const
{
    return !m_MainDir.empty();
}

void Project::outputFile(const std::string &file, const std::vector<unsigned char>& data, size_t length, int start)
{
    std::ofstream output(
                file,
                std::ios::binary | std::ios::out
                );
    if (!output) {
        throw ASMError("Could not open " + file + " for writing");
    }
    output.write((char*)&(data[start]), length);
    output.close();
}

bool Project::validateConfig(const YAML::Node &configYML)
{
    std::ostringstream errorString;
    bool isValid = true;
    if (!configYML[FILES_SECTION].IsDefined() || !configYML[FILES_SECTION].IsMap()) {
        isValid = false;
        errorString << "files section is missing or not a map.\n";
    } else {
        if (!configYML[FILES_SECTION][DIR_MAIN].IsDefined()
                || !configYML[FILES_SECTION][DIR_MAIN].IsScalar()) {
            isValid = false;
            errorString << "main directory for project is missing from files config.\n";
        }
        if (!configYML[FILES_SECTION][INPUT_SECTION].IsDefined()
                || !configYML[FILES_SECTION][INPUT_SECTION].IsMap()) {
            isValid = false;
            errorString << "input section is missing or not a map.\n";
        } else {
            if (!configYML[FILES_SECTION][INPUT_SECTION][DIR_VAL].IsDefined()
                    || !configYML[FILES_SECTION][INPUT_SECTION][DIR_VAL].IsScalar()) {
                isValid = false;
                errorString << "input directory for project is missing from files config.\n";
            }
        }
        if (!configYML[FILES_SECTION][OUTPUT_SECTION].IsDefined() || !configYML[FILES_SECTION][OUTPUT_SECTION].IsMap()) {
            isValid = false;
            errorString << "output section is missing or not a map.\n";
        } else {
            YAML::Node outputConfig = configYML[FILES_SECTION][OUTPUT_SECTION];
            if (!outputConfig[DIR_VAL].IsDefined() || !outputConfig[DIR_VAL].IsScalar()) {
                isValid = false;
                errorString << "output directory for project is missing from files config.\n";
            }
            if (!outputConfig[OUTPUT_BIN].IsDefined() || !outputConfig[OUTPUT_BIN].IsMap()) {
                isValid = false;
                errorString << "output binaries subdirectory section is missing or not a map.\n";
            } else {
                if (!outputConfig[OUTPUT_BIN][DIR_MAIN].IsDefined() || !outputConfig[OUTPUT_BIN][DIR_MAIN].IsScalar()) {
                    isValid = false;
                    errorString << "output binaries subdirectory mainDir value is missing from files config.\n";
                }
                if (!outputConfig[OUTPUT_BIN][DIR_TEXT].IsDefined() || !outputConfig[OUTPUT_BIN][DIR_TEXT].IsScalar()) {
                    isValid = false;
                    errorString << "output binaries subdirectory textDir value is missing from files config.\n";
                }
                if (outputConfig[OUTPUT_BIN][EXTRAS].IsDefined() && !outputConfig[OUTPUT_BIN][EXTRAS].IsSequence()) {
                    isValid = false;
                    errorString << "extras section for output binaries must be a sequence.\n";
                }
                if (!outputConfig[OUTPUT_BIN][FONT_SECTION].IsDefined()
                        || !outputConfig[OUTPUT_BIN][FONT_SECTION].IsMap())
                {
                    isValid = false;
                    errorString << "fonts section for output binaries is missing or not a map.\n";
                } else {
                    if (!outputConfig[OUTPUT_BIN][FONT_SECTION][DIR_FONT].IsScalar()) {
                        isValid = false;
                        errorString << "fonts directory must be a scalar.\n";
                    }
                    if (!outputConfig[OUTPUT_BIN][FONT_SECTION][INCLUDE_VAL].IsSequence()) {
                        isValid = false;
                        errorString << "fonts includes must be a sequence.\n";
                    }
                }
            }
            if (outputConfig[INCLUDE_VAL].IsDefined() && !outputConfig[INCLUDE_VAL].IsSequence()) {
                isValid = false;
                errorString << "includes section for output must be a sequence.\n";
            }
        }
        if (!configYML[FILES_SECTION][DIR_ROM].IsDefined() || !configYML[FILES_SECTION][DIR_ROM].IsScalar()) {
            isValid = false;
            errorString << "romDir for project is missing from files config.\n";
        }
    }
    if (!configYML[CONFIG_SECTION].IsDefined() || !configYML[CONFIG_SECTION].IsMap()) {
        isValid = false;
        errorString << "config section is missing or not a map.\n";
    } else {
        if (!configYML[CONFIG_SECTION][DIR_VAL].IsDefined() || !configYML[CONFIG_SECTION][DIR_VAL].IsScalar()) {
            isValid = false;
            errorString << "directory for config section is missing or is not a scalar.\n";
        }
        if (!configYML[CONFIG_SECTION][IN_MAP].IsDefined() || !configYML[CONFIG_SECTION][IN_MAP].IsScalar()) {
            isValid = false;
            errorString << "inMapping for config section is missing or is not a scalar.\n";
        }
        if (configYML[CONFIG_SECTION][MAP_TYPE].IsDefined() && !configYML[CONFIG_SECTION][MAP_TYPE].IsScalar()) {;
            isValid = false;
            errorString << "config > mapper must be a string.\n";
        }
    }
    if (!configYML[ROMS].IsDefined()) {
        isValid = false;
        errorString << "roms section is missing.\n";
    } else {
        if (!configYML[ROMS].IsSequence()) {
            isValid = false;
            errorString << "roms section in config must be a sequence.\n";
        } else {
            int romindex = 0;
            for(auto node: configYML[ROMS]) {
                std::string romName;
                if (!node["name"].IsDefined() || !node["name"].IsScalar()) {
                    errorString << "rom at index " << romindex << " is missing a name value.\n";
                    char temp[50];
                    sprintf(temp, "at index %d", romindex);
                    romName = temp;
                    isValid = false;
                } else {
                    romName = node["name"].Scalar();
                }
                if (!node["file"].IsDefined() || !node["file"].IsScalar()) {
                    errorString << "rom " << romName << " is missing a file value.\n";
                    isValid = false;
                }
                if (node["header"].IsDefined() && (!node["header"].IsScalar() || (
                node["header"].Scalar() != "auto" && node["header"].Scalar() != "true" && node["header"].Scalar() != "false"))) {
                    errorString << "rom " << romName << " does not have a valid header option - must be \"true\", \"false\", \"auto\", or not defined.\n";
                    isValid = false;
                }
            }
        }
    }
    if (!isValid) {
        throw ConfigError(errorString.str());
    }
    return isValid;
}

ConfigError::ConfigError(std::string message) : std::runtime_error(message) {}
ASMError::ASMError(std::string message) : std::runtime_error(message) {}
ParseError::ParseError(std::string message) : std::runtime_error(message) {}
}

bool YAML::convert<sable::Project::Rom>::decode(const Node &node, sable::Project::Rom &rhs)
{
    rhs.name = node["name"].as<std::string>();
    rhs.file = node["file"].as<std::string>();
    rhs.hasHeader = 0;
    if (node["header"].IsDefined()) {
        std::string tmp = node["header"].as<std::string>();
        if (tmp == "true") {
            rhs.hasHeader = 1;
        } else if (tmp == "false") {
            rhs.hasHeader = -1;
        } else if (tmp == "auto") {
            rhs.hasHeader = 0;
        } else {
            return false;
        }
    }
    if (node["includes"].IsDefined() && node["includes"].IsSequence()) {
        rhs.includes = node["includes"].as<std::vector<std::string>>();
    }
    return true;
}
