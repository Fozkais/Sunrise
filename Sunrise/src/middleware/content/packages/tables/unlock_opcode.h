#pragma once

#include <cstdint>

namespace sunrise::middleware::content::packages::tables {

/** Native instruction values before unlock references are bound. */
enum class UnlockOpcode : std::uint32_t {
    flag = 1,
    logicalNot = 2,
    logicalOr = 3,
    logicalAnd = 4,
    logicalNor = 5,
    notEqualAlternate = 6,
    logicalNand = 7,
    equal = 8,
    notEqual = 9,
    loadValue = 10,
    constant = 11,
    expression = 12,
    greaterThan = 13,
    greaterOrEqual = 14,
    lessThan = 15,
    lessOrEqual = 16,
    add = 17,
    subtract = 18,
    multiply = 19,
    divide = 20,
    remainder = 21,
    negate = 22,
    hash = 23,
    hashCombine = 24,
    bitwiseAnd = 25,
    bitwiseOr = 26,
    bitwiseXor = 27,
    bitwiseNot = 28,
};

} // namespace sunrise::middleware::content::packages::tables
