/* GNU driver response files use whitespace, quotes and backslash escapes. */
#pragma once
#include "rust_common.h"
namespace rust {
inline Words link_response_words(const string &text) {
    Words words;
    string word;
    char quote = 0;
    bool have = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (!quote && isspace(static_cast<unsigned char>(c))) {
            if (have) {
                words.push_back(word);
                word.clear();
                have = false;
            }
        } else if (c == '\\' && quote != '\'') {
            if (++i == text.size())
                throw Local("incomplete linker response escape");
            word += text[i];
            have = true;
        } else if (quote && quote == c)
            quote = 0;
        else if (!quote && (c == '\'' || c == '"')) {
            quote = c;
            have = true;
        } else {
            word += c;
            have = true;
        }
    }
    if (quote)
        throw Local("incomplete linker response quote");
    if (have)
        words.push_back(word);
    return words;
}
inline void expand_link_args(const Words &args, Words &flat, std::set<fs::path> &active,
                             std::set<fs::path> *files = nullptr) {
    for (const auto &arg : args) {
        if (arg.size() < 2 || arg.front() != '@') {
            flat.push_back(arg);
            continue;
        }
        auto spelling = fs::absolute(arg.substr(1)).lexically_normal(), file = fs::canonical(spelling);
        if (!active.insert(file).second || active.size() > 128)
            throw Local("recursive linker response");
        if (files)
            files->insert(spelling);
        expand_link_args(link_response_words(read(file)), flat, active, files);
        active.erase(file);
    }
}
} // namespace rust
