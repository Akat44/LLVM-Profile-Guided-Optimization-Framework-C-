#pragma once

#include <string>
#include <unordered_map>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>

#include "json.hpp"

using nlohmann::json;

struct BlockRecord {
	uint64_t num_entries;
	std::unordered_map<std::string, uint64_t> from;
	std::unordered_map<std::string, uint64_t> to;
};

struct FunctionRecord {
	uint64_t num_entries;
	std::unordered_map<std::string, uint64_t> from;
	std::unordered_map<std::string, uint64_t> to;
};

struct ProfileData {
	std::unordered_map<std::string, BlockRecord> block_records;
	std::unordered_map<std::string, FunctionRecord> function_records;
};

void from_json(const json& j, BlockRecord& record) {
	j.at("num_entries").get_to(record.num_entries);
	j.at("from").get_to(record.from);
	j.at("to").get_to(record.to);
}

void from_json(const json& j, FunctionRecord& record) {
	j.at("num_entries").get_to(record.num_entries);
	j.at("from").get_to(record.from);
	j.at("to").get_to(record.to);
}

void from_json(const json& j, ProfileData& data) {
	j.at("block_records").get_to(data.block_records);
	j.at("function_records").get_to(data.function_records);
}

std::string get_block_name(llvm::BasicBlock& block) {
	return block.getParent()->getNameOrAsOperand() + ": " + block.getNameOrAsOperand();
}

std::string get_func_name(llvm::Function& func) {
	return func.getNameOrAsOperand();
}

ProfileData profile_data_from(std::istream& in) {
	return json::parse(in).get<ProfileData>();
}