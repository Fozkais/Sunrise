#pragma once

#include "../../../../middleware/content/packages/reader/reader.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../state/build_data/rewards/definition.h"
#include "state/account/account_state.h"

namespace sunrise::client::content::items::packages {

struct SlotMaps;

/** Resolves expression references and unlock slots once for a package pass. */
class RewardConditions {
public:
    [[nodiscard]] bool load(const middleware::content::packages::reader::Source& source,
                            middleware::content::packages::reader::Scratch& scratch,
                            std::span<const std::byte> root,
                            const SlotMaps& maps) noexcept;
    [[nodiscard]] bool bind(state::build_data::rewards::Instruction& instruction) const noexcept;
    [[nodiscard]] bool read(std::span<const std::byte> blob,
                            std::size_t at,
                            std::vector<state::build_data::rewards::Instruction>& bank,
                            state::build_data::rewards::Range& range) const noexcept;
    [[nodiscard]] bool read_list(std::span<const std::byte> blob,
                                 std::size_t at,
                                 std::span<state::build_data::rewards::Instruction> output,
                                 std::size_t& count) const noexcept;

private:
    [[nodiscard]] bool append_expression(std::span<const std::byte> blob,
                                         std::size_t at,
                                         std::vector<state::build_data::rewards::Instruction>& bank,
                                         std::size_t depth) const noexcept;
    void load_class_flags(const middleware::content::packages::reader::Source& source,
                          middleware::content::packages::reader::Scratch& scratch,
                          std::span<const std::byte> root) noexcept;
    std::array<std::uint16_t, state::kCharacterClassCount> classFlags_{};
    const SlotMaps* maps_{};
    std::vector<std::byte> flags_;
    std::vector<std::byte> values_;
    std::vector<std::byte> expressions_;
    middleware::content::packages::tables::Array flagRows_{};
    middleware::content::packages::tables::Array valueRows_{};
    middleware::content::packages::tables::Array expressionRows_{};
};

/** Reward metadata collected alongside the existing item-definition walk. */
class RewardBuild {
public:
    RewardConditions conditions;
    [[nodiscard]] bool load(const middleware::content::packages::reader::Source& source,
                            middleware::content::packages::reader::Scratch& scratch,
                            std::span<const std::byte> root,
                            const SlotMaps& maps) noexcept;
    [[nodiscard]] bool begin_items(std::size_t count) noexcept;
    void item(std::uint16_t index, std::uint32_t hash, std::span<const std::byte> blob) noexcept;
    [[nodiscard]] bool publish() noexcept;

private:
    [[nodiscard]] bool entry(std::span<const std::byte> blob, std::size_t at) noexcept;
    [[nodiscard]] bool read_item(std::uint32_t hash,
                                 std::span<const std::byte> blob,
                                 state::build_data::rewards::Item& item) noexcept;
    bool loaded_{};
    std::vector<state::build_data::rewards::Pool> pools;
    std::vector<state::build_data::rewards::Entry> entries;
    std::vector<state::build_data::rewards::Item> items;
    std::vector<state::build_data::rewards::Instruction> instructions;
    std::vector<state::build_data::rewards::Modifier> modifiers;
    std::vector<state::build_data::rewards::SocketOverride> sockets;
};

/** Progression rewards and pool entries use the same socket-override layout. */
[[nodiscard]] bool read_reward_sockets(std::span<const std::byte> blob,
                                       std::size_t at,
                                       std::span<state::build_data::rewards::SocketOverride> output,
                                       std::size_t& count) noexcept;

} // namespace sunrise::client::content::items::packages
