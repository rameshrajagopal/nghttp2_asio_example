#ifndef _NGHTTP_CONFIG_H_INCLUDED_
#define _NGHTTP_CONFIG_H_INCLUDED_

#include <iostream>
#include <string>
#include <sstream>
#include <map>
#include <fstream>
#include <typeinfo>

#define  MASTER_NODE_ADDR  "192.168.0.241"
#define  MASTER_NODE_PORT  "8000"
#define  SEMICOLON         ":"
#define  SLASH             "/"
#define  MASTER_PORT       MASTER_NODE_ADDR SEMICOLON MASTER_NODE_PORT SLASH
#define  PROTOCOL          "http://"
#define  MASTER_NODE_URI   PROTOCOL MASTER_PORT
#define  WORKER_FILE       "/work"
#define within(num) (int) ((float) num * random() / (RAND_MAX + 1.0))
#define SYSLOG(fmt...)    syslog(fmt)

#define MAX_NUM_SLAVES  (3)
typedef struct {
    std::string addr;
    std::string port;
    std::string uri;
}SlaveAddr;

#define SLAVE_ADDR  "192.168.0.241"
#define SLAVE_PORT  "7000"
#define SLAVE1_ADDR "192.168.0.12"

void exitWithError(const std::string &error) ;

class Convert
{
public:
	template <typename T>
	static std::string T_to_string(T const &val)
	{
		std::ostringstream ostr;
		ostr << val;

		return ostr.str();
	}

	template <typename T>
	static T string_to_T(std::string const &val)
	{
		std::istringstream istr(val);
		T returnVal;
		if (!(istr >> returnVal))
			exitWithError("CFG: Not a valid " + (std::string)typeid(T).name() + " received!\n");

		return returnVal;
	}

	static std::string string_to_T(std::string const &val)
	{
		return val;
	}
};


class ConfigFile
{
private:
	std::map<std::string, std::string> contents;
	std::string fName;

	void removeComment(std::string &line) const
	{
		if (line.find(';') != line.npos)
			line.erase(line.find(';'));
	}

	bool onlyWhitespace(const std::string &line) const
	{
		return (line.find_first_not_of(' ') == line.npos);
	}
	bool validLine(const std::string &line) const
	{
		std::string temp = line;
		temp.erase(0, temp.find_first_not_of("\t "));
		if (temp[0] == '=')
			return false;

		for (size_t i = temp.find('=') + 1; i < temp.length(); i++)
			if (temp[i] != ' ')
				return true;

		return false;
	}

	void extractKey(std::string &key, size_t const &sepPos, const std::string &line) const
	{
		key = line.substr(0, sepPos);
		if (key.find('\t') != line.npos || key.find(' ') != line.npos)
			key.erase(key.find_first_of("\t "));
	}
	void extractValue(std::string &value, size_t const &sepPos, const std::string &line) const
	{
		value = line.substr(sepPos + 1);
		value.erase(0, value.find_first_not_of("\t "));
		value.erase(value.find_last_not_of("\t ") + 1);
	}

	void extractContents(const std::string &line)
	{
		std::string temp = line;
		temp.erase(0, temp.find_first_not_of("\t "));
		size_t sepPos = temp.find('=');

		std::string key, value;
		extractKey(key, sepPos, temp);
		extractValue(value, sepPos, temp);

		if (!keyExists(key))
			contents.insert(std::pair<std::string, std::string>(key, value));
		else
			exitWithError("CFG: Can only have unique key names!\n");
	}

	void parseLine(const std::string &line, size_t const lineNo)
	{
		if (line.find('=') == line.npos)
			exitWithError("CFG: Couldn't find separator on line: " + Convert::T_to_string(lineNo) + "\n");

		if (!validLine(line))
			exitWithError("CFG: Bad format for line: " + Convert::T_to_string(lineNo) + "\n");

		extractContents(line);
	}

	void ExtractKeys()
	{
		std::ifstream file;
		file.open(fName.c_str());
		if (!file)
			exitWithError("CFG: File " + fName + " couldn't be found!\n");

		std::string line;
		size_t lineNo = 0;
		while (std::getline(file, line))
		{
			lineNo++;
			std::string temp = line;

			if (temp.empty())
				continue;

			removeComment(temp);
			if (onlyWhitespace(temp))
				continue;

			parseLine(temp, lineNo);
		}

		file.close();
	}
public:
	ConfigFile(const std::string &fName)
	{
		this->fName = fName;
		ExtractKeys();
	}

	bool keyExists(const std::string &key) const
	{
		return contents.find(key) != contents.end();
	}

	template <typename ValueType>
	ValueType getValueOfKey(const std::string &key, ValueType const &defaultValue = ValueType()) const
	{
		if (!keyExists(key))
			return defaultValue;

		return Convert::string_to_T<ValueType>(contents.find(key)->second);
	}
    void printAll() {
        auto it = contents.begin();
        for (; it != contents.end(); ++it) {
            std::cout << it->first << " " << it->second << std::endl;
        }
    }
};


#endif /*_NGHTTP_CONFIG_H_INCLUDED_*/
