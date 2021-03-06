#ifndef PARSE_H
#define PARSE_H

#include <istream>
#include <map>
#include <string>
#include <yaml-cpp/yaml.h>
#include "font.h"
#include <tuple>

namespace sable {
    typedef std::back_insert_iterator<std::vector<unsigned char>> back_inserter;
    struct ParseSettings {
        bool autoend, printpc;
        std::string mode, label;
        int maxWidth;
        int currentAddress;
    };

    class TextParser
    {
    public:
        TextParser()=default;
        TextParser(const YAML::Node& node, const std::string& defaultMode, const std::string& newlineName = "NewLine");
        struct lineNode{
            bool hasNewLines;
            int length;
            std::vector<unsigned char> data;
        };

        std::pair<bool, int> parseLine(std::istream &input, ParseSettings &settings, back_inserter insert);
        const std::map<std::string, Font>& getFonts() const;
        static void insertData(unsigned int code, int size, back_inserter bi);
        ParseSettings updateSettings(const ParseSettings &settings, const std::string& setting = "", unsigned int currentAddress = 0);
        ParseSettings getDefaultSetting(int address);
    private:
        bool useDigraphs;
        int maxWidth;
        std::map<std::string, Font> m_Fonts;
        std::string defaultFont, newLineName;
        static std::string readUtf8Char(std::string::iterator& start, std::string::iterator end, bool advance = true);
    };
}


#endif // PARSE_H
