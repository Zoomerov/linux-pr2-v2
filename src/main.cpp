#include "repository.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

struct Options {
    std::string package;
    std::string repository;
    fs::path output;
    bool test = false;
    bool order = false;
};

static void usage(std::ostream &out) {
    out << "Usage: nuget-graph --package ID --repository URL|FILE --output FILE.png "
           "[--test] [--order]\n";
}

static Options parseArgs(int argc, char **argv) {
    Options options;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            std::exit(0);
        }
        if (!seen.insert(arg).second) {
            throw std::runtime_error("Duplicate option: " + arg);
        }
        if (arg == "--test" || arg == "--order") {
            if (arg == "--test") options.test = true;
            else options.order = true;
            continue;
        }
        if (arg != "--package" && arg != "--repository" && arg != "--output") {
            throw std::runtime_error("Unknown option: " + arg);
        }
        if (++i >= argc || argv[i][0] == '\0') {
            throw std::runtime_error("Missing value for " + arg);
        }
        if (arg == "--package") options.package = argv[i];
        if (arg == "--repository") options.repository = argv[i];
        if (arg == "--output") options.output = fs::path(argv[i]);
    }
    if (options.package.empty() || options.repository.empty() || options.output.empty()) {
        throw std::runtime_error("--package, --repository and --output are required");
    }
    if (options.output.extension() != ".png") {
        throw std::runtime_error("--output must end in .png");
    }
    if (options.test && !isTestPackage(options.package)) {
        throw std::runtime_error("In --test mode, --package must contain only A-Z");
    }
    if (!options.test && !isNuGetId(options.package)) {
        throw std::runtime_error("Invalid NuGet package ID");
    }
    return options;
}

struct Graph {
    std::set<std::string> nodes;
    std::set<std::pair<std::string, std::string>> edges;
};

static Graph buildGraph(Repository &repository, const std::string &root) {
    Graph graph;
    std::queue<std::string> pending;
    std::set<std::string> visited;
    graph.nodes.insert(root);
    pending.push(root);
    while (!pending.empty()) {
        const std::string current = pending.front();
        pending.pop();
        if (!visited.insert(current).second) continue;
        const auto direct = repository.dependencies(current);
        if (current == root) {
            std::cout << "Direct dependencies:\n";
            if (direct.empty()) std::cout << "(none)\n";
            else for (const auto &dependency : direct) std::cout << dependency << '\n';
        }
        for (const auto &dependency : direct) {
            graph.nodes.insert(dependency);
            graph.edges.insert({current, dependency});
            if (!visited.contains(dependency)) pending.push(dependency);
        }
    }
    return graph;
}

static std::vector<std::string> loadingOrder(const Graph &graph) {
    std::map<std::string, int> remaining;
    std::map<std::string, std::set<std::string>> dependents;
    for (const auto &node : graph.nodes) remaining[node] = 0;
    for (const auto &[package, dependency] : graph.edges) {
        ++remaining[package];
        dependents[dependency].insert(package);
    }
    std::set<std::string> ready;
    for (const auto &[node, count] : remaining) {
        if (count == 0) ready.insert(node);
    }
    std::vector<std::string> order;
    while (!ready.empty()) {
        const std::string next = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(next);
        for (const auto &dependent : dependents[next]) {
            if (--remaining[dependent] == 0) ready.insert(dependent);
        }
    }
    if (order.size() != graph.nodes.size()) {
        throw std::runtime_error("Dependency cycle: no valid loading order exists");
    }
    return order;
}

static std::string quoted(const std::string &value) {
    std::string result = "\"";
    for (char ch : value) {
        if (ch == '\\' || ch == '"') result += '\\';
        if (ch == '\n' || ch == '\r') result += ' ';
        else result += ch;
    }
    return result + '"';
}

static std::string d2Text(const Graph &graph) {
    std::string result = "direction: right\n";
    for (const auto &node : graph.nodes) result += quoted(node) + "\n";
    for (const auto &[source, target] : graph.edges) {
        result += quoted(source) + " -> " + quoted(target) + "\n";
    }
    return result;
}

static void writeDot(const Graph &graph, const fs::path &path) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot write " + path.string());
    out << "digraph dependencies {\n  rankdir=LR;\n  node [shape=box];\n";
    for (const auto &node : graph.nodes) out << "  " << quoted(node) << ";\n";
    for (const auto &[source, target] : graph.edges) {
        out << "  " << quoted(source) << " -> " << quoted(target) << ";\n";
    }
    out << "}\n";
}

static int runD2(const fs::path &source, const fs::path &output) {
#ifdef _WIN32
    return _spawnlp(_P_WAIT, "d2", "d2", source.string().c_str(),
                    output.string().c_str(), nullptr);
#else
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("Cannot start D2");
    if (child == 0) {
        execlp("d2", "d2", source.c_str(), output.c_str(),
               static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static int runGraphviz(const fs::path &source, const fs::path &output) {
#ifdef _WIN32
    return _spawnlp(_P_WAIT, "dot", "dot", "-Tpng", source.string().c_str(),
                    "-o", output.string().c_str(), nullptr);
#else
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("Cannot start Graphviz");
    if (child == 0) {
        execlp("dot", "dot", "-Tpng", source.c_str(), "-o", output.c_str(),
               static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static fs::path saveGraph(const Graph &graph, const fs::path &output) {
    if (output.has_parent_path()) fs::create_directories(output.parent_path());
    fs::path d2 = output;
    d2.replace_extension(".d2");
    std::ofstream d2File(d2);
    if (!d2File) throw std::runtime_error("Cannot write " + d2.string());
    d2File << d2Text(graph);
    d2File.close();
    const int d2Status = runD2(d2, output);
    if (d2Status == 0) return d2;
    if (d2Status != 127 && d2Status != -1) {
        throw std::runtime_error("D2 failed to render " + d2.string());
    }
    fs::path dot = output;
    dot += ".dot.tmp";
    writeDot(graph, dot);
    const int status = runGraphviz(dot, output);
    fs::remove(dot);
    if (status != 0) throw std::runtime_error("Graphviz 'dot' failed; install Graphviz");
    return d2;
}

int main(int argc, char **argv) {
    try {
        const Options options = parseArgs(argc, argv);
        std::cout << "package=" << options.package << "\nrepository=" << options.repository
                  << "\ntest=" << (options.test ? "true" : "false")
                  << "\noutput=" << options.output.string()
                  << "\norder=" << (options.order ? "true" : "false") << '\n';
        Repository repository(options.repository, options.test);
        std::string root = options.package;
        if (!options.test) {
            std::transform(root.begin(), root.end(), root.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
        }
        const Graph graph = buildGraph(repository, root);
        if (options.order) {
            const auto order = loadingOrder(graph);
            std::cout << "Loading order:\n";
            for (const auto &name : order) std::cout << name << '\n';
        }
        const fs::path d2 = saveGraph(graph, options.output);
        std::cout << d2Text(graph);
        std::cout << "Saved " << graph.nodes.size() << " packages, " << graph.edges.size()
                  << " links to " << options.output << " and " << d2 << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Error: " << error.what() << '\n';
        usage(std::cerr);
        return 1;
    }
}
