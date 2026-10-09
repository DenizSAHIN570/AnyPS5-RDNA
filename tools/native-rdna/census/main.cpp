#include "Census.hpp"
#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<std::byte> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path.string());
    std::vector<char> text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(text.size());
    std::transform(text.begin(), text.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

std::vector<std::filesystem::path> collect(const std::vector<std::string>& inputs) {
    std::vector<std::filesystem::path> files;
    for (const auto& input : inputs) {
        const std::filesystem::path path(input);
        if (std::filesystem::is_directory(path)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path))
                if (entry.is_regular_file() && entry.path().extension() == ".ash") files.push_back(entry.path());
        } else if (std::filesystem::is_regular_file(path)) {
            files.push_back(path);
        } else {
            throw std::runtime_error("no such file or directory: " + input);
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

}

int main(int argc, char** argv) try {
    std::string output;
    std::vector<std::string> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-o") {
            if (++i == argc) throw std::runtime_error("-o needs a file name");
            output = argv[i];
        } else {
            inputs.push_back(argument);
        }
    }
    if (inputs.empty()) {
        std::fprintf(stderr, "usage: shader_census [-o census.json] <dump directory | shader_*.ash>...\n"
                             "  dumps come from APS5_DUMP_REGISTERED_SHADERS=<directory>\n");
        return 2;
    }
    ShaderCensus::Census census;
    for (const auto& path : collect(inputs)) {
        const auto bytes = readFile(path);
        ShaderCensus::AddShader(census, path.filename().string(), ShaderCensus::ParseShaderDump(bytes));
    }
    const auto json = ShaderCensus::ToJson(census);
    if (output.empty()) {
        std::fwrite(json.data(), 1, json.size(), stdout);
    } else {
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("cannot open " + output);
        file.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!file) throw std::runtime_error("cannot write " + output);
    }
    std::size_t failed = 0;
    for (const auto& shader : census.shaders)
        if (!shader.decodeError.empty()) ++failed;
    std::fprintf(stderr, "%zu shaders, %zu opcodes, %zu shaders failed to decode\n", census.shaders.size(), census.opcodes.size(), failed);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "shader_census: %s\n", error.what());
    return 1;
}
