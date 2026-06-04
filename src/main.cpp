#include "Lexer.h"
#include "Parser.h"
#include "ASTPrinter.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char* argv[]) {
    std::string source;

    if (argc >= 2) {
        // 从文件读取
        std::ifstream file(argv[1]);
        if (!file) {
            std::cerr << "Error: cannot open file '" << argv[1] << "'\n";
            return 1;
        }
        std::ostringstream oss;
        oss << file.rdbuf();
        source = oss.str();
    } else {
        // 默认测试代码（README 第13节完整示例）
        source = R"xxx(
type User = { id: int, name: string }

interface Stringer {
    to_string() -> string
}

fun (self User) User(id: int, name: string) {
    self.id = id
    self.name = name
}

fun (self User impl Stringer) to_string() -> string {
    return "User(" + self.id + ", " + self.name + ")"
}

type Pair = { first: <A>, second: <B> }

fun zip(a: <A>, b: <B>) -> Pair {
    return Pair(a, b)
}

fun parseUser(json: string) throws -> User {
    if json == "" {
        throw { kind = "parse_error", message = "empty json" }
    }
    let obj = parseJson(json)!
    return User(obj.id, obj.name)
}

fun loadUser(id: int, io: Io) throws -> User {
    let filename = "user_" + id + ".json"
    let data = io.readFile(filename)!
    return parseUser(data)!
}

fun getUserSafe(id: int, io: Io) throws -> User | None {
    if io.fileExists("user_" + id + ".json") {
        return loadUser(id, io)!
    }
    return None
}

fun main(io: Io) throws {
    let p = zip(42, "hello")
    io.println("Pair: (" + p.first + ", " + p.second + ")")

    let ids = [1, 2, 3]
    sync {
        for id in ids {
            spawn {
                match getUserSafe(id, io) {
                    User u => io.println(u.to_string()),
                    None   => io.println("User " + id + " not found")
                }
            }
        }
    }

    try {
        let test = loadUser(999, io)!
        io.println(test.to_string())
    } catch (e) {
        io.println("Failed to load user: " + e.message)
    }

    io.println("All tasks completed")
}
)xxx";
    }

    // 1. 词法分析
    Aura::Lexer lexer(source);
    auto tokens = lexer.scanAll();

    std::cout << "=== Tokens (" << tokens.size() << ") ===" << std::endl;
    for (auto& tok : tokens) {
        std::cout << tok << '\n';
    }
    std::cout << std::flush;

    // 2. 语法分析
    Aura::Parser parser(std::move(tokens));
    std::cout << "=== Parse ===" << std::endl;
    auto program = parser.parse();
    std::cout << "=== Parse done ===" << std::endl;

    if (!parser.errors().empty()) {
        std::cerr << "\n=== Parse Errors ===\n";
        for (auto& err : parser.errors()) {
            std::cerr << err << '\n';
        }
        return 1;
    }

    // 3. 打印 AST
    std::cout << "\n=== AST ===\n";
    if (program) {
        program->print(std::cout, 0);
    } else {
        std::cerr << "Failed to parse program.\n";
        return 1;
    }

    return 0;
}
