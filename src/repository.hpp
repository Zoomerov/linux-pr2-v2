#pragma once

#include <map>
#include <string>
#include <vector>

bool isTestPackage(const std::string &name);
bool isNuGetId(const std::string &name);

class Repository {
public:
    Repository(const std::string &source, bool test);
    std::vector<std::string> dependencies(const std::string &package);

private:
    std::string registrationBase_;
    bool test_;
    std::map<std::string, std::vector<std::string>> testGraph_;
    std::map<std::string, std::vector<std::string>> cache_;
};
