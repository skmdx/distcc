/* GNU driver response files use whitespace, quotes and backslash escapes. */
#pragma once
#include "rust/common.hh"
#include <set>
namespace rust {
enum class ResponseSyntax { RustShell, Linker };
inline Words response_words(const string &text, ResponseSyntax syntax) {
    bool shell = syntax == ResponseSyntax::RustShell;
    Words args;
    string word;
    char quote = 0;
    bool started = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (!quote &&
            (shell ? (c == ' ' || c == '\t' || c == '\n') : bool(isspace(static_cast<unsigned char>(c))))) {
            if (started) {
                args.push_back(word);
                word.clear();
                started = false;
            }
        } else if (shell && !quote && c == '#' && !started) {
            i = text.find('\n', i);
            if (i == string::npos)
                break;
        } else if (c == '\\' && quote != '\'') {
            if (++i == text.size())
                throw Local("unterminated response file escape");
            c = text[i];
            if (shell && c == '\n')
                continue;
            if (shell && quote && c != '$' && c != '`' && c != '"' && c != '\\')
                word += '\\';
            word += c;
            started = true;
        } else if (quote && c == quote)
            quote = 0;
        else if (!quote && (c == '\'' || c == '"')) {
            quote = c;
            started = true;
        } else {
            word += c;
            started = true;
        }
    }
    if (quote)
        throw Local("unterminated response file quote");
    if (started)
        args.push_back(word);
    return args;
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
        expand_link_args(response_words(read(file), ResponseSyntax::Linker), flat, active, files);
        active.erase(file);
    }
}
} // namespace rust
