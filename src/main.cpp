#include "minidb.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string readFile(const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("could not open SQL file: " + path);
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

void runStatements(minidb::Database& db, const std::string& text) {
  for (const auto& statement : minidb::splitStatements(text)) {
    if (minidb::trim(statement).empty()) {
      continue;
    }
    std::cout << "sql> " << minidb::trim(statement) << ";\n";
    try {
      std::cout << db.execute(statement) << "\n\n";
    } catch (const std::exception& ex) {
      std::cout << "ERROR: " << ex.what() << "\n\n";
    }
  }
}

}

int main(int argc, char** argv) {
  std::string dataDir = "data";
  std::string filePath;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--data" && i + 1 < argc) {
      dataDir = argv[++i];
    } else if (arg == "--file" && i + 1 < argc) {
      filePath = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: minisql [--data DIR] [--file script.sql]\n";
      return 0;
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      return 1;
    }
  }

  try {
    minidb::Database db(dataDir);
    db.load();

    if (!filePath.empty()) {
      runStatements(db, readFile(filePath));
      return 0;
    }

    std::cout << "MiniSQL Advanced Engine. End SQL with ';'. Type .exit to quit.\n";
    std::string line;
    std::string buffer;
    while (std::cout << "sql> " && std::getline(std::cin, line)) {
      if (minidb::trim(line) == ".exit") {
        break;
      }
      buffer += line + "\n";
      if (buffer.find(';') != std::string::npos) {
        runStatements(db, buffer);
        buffer.clear();
      }
    }
  } catch (const std::exception& ex) {
    std::cerr << "fatal: " << ex.what() << "\n";
    return 1;
  }

  return 0;
}
