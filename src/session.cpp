/*
 * Copyright (c) 2011-2014 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin-node.
 *
 * libbitcoin-node is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/node/session.hpp>

#include <bitcoin/blockchain.hpp>

namespace libbitcoin {
namespace node {

using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;
using std::placeholders::_4;

session::session(threadpool& pool, const session_params& params)
  : strand_(pool.service()),
    handshake_(params.handshake_), protocol_(params.protocol_),
    chain_(params.blockchain_), poll_(params.poller_),
    tx_pool_(params.transaction_pool_),
    grabbed_invs_(20)
{
}

void handle_handshake_height_set(const std::error_code&)
{
    // Start height now set in handshake
    // Do nothing
}
void session::start(completion_handler handle_complete)
{
    protocol_.start(handle_complete);
    protocol_.subscribe_channel(
        std::bind(&session::new_channel, this, _1, _2));

    // set_start_height expects uint32_t but fetch_last_height returns
    // height as uint64_t. This results in integer narrowing compile warnings.
    // This results from the satoshi version structure expecting uint32_t but
    // block heights capable of supporting a full uint64_t range (via varint).
    // The warnings could be resolve through an indirection, but the logical
    // inconsistency would remain. That issue won't become a problem until
    // the year ~ 3375. By that time bender could fix it.
    chain_.fetch_last_height(
        std::bind(&network::handshake::set_start_height,
            &handshake_, _2, handle_handshake_height_set));

    chain_.subscribe_reorganize(
        std::bind(&session::set_start_height,
            this, _1, _2, _3, _4));
}

void session::stop(completion_handler handle_complete)
{
    protocol_.stop(handle_complete);
}

void session::new_channel(const std::error_code& ec, network::channel_ptr node)
{
    if (ec)
    {
        log_warning(LOG_SESSION) << "New channel: " << ec.message();
        return;
    }
    BITCOIN_ASSERT(node);
    node->subscribe_inventory(
        std::bind(&session::inventory, this, _1, _2, node));
    node->subscribe_get_blocks(
        std::bind(&session::get_blocks, this, _1, _2, node));
    // tx
    // block
    protocol_.subscribe_channel(
        std::bind(&session::new_channel, this, _1, _2));
    poll_.query(node);
    poll_.monitor(node);
}

// There is an inconsistency in the wire protocols where fetch_last_height
// requires a uint64_t callback but set_start_height is uint32_t.
void session::set_start_height(const std::error_code& ec, uint64_t fork_point,
    const chain::blockchain::block_list& new_blocks,
    const chain::blockchain::block_list& /* replaced_blocks */)
{
    if (ec)
    {
        BITCOIN_ASSERT(ec == error::service_stopped);
        return;
    }

    // Start height is limited to max_uint32 by satoshi protocol (version).
    BITCOIN_ASSERT((bc::max_uint32 - fork_point) >= new_blocks.size());
    auto start_height = static_cast<uint32_t>(fork_point + new_blocks.size());

    handshake_.set_start_height(start_height, handle_handshake_height_set);
    chain_.subscribe_reorganize(
        std::bind(&session::set_start_height,
            this, _1, _2, _3, _4));

    // Broadcast invs of new blocks
    inventory_type blocks_inv;
    for (auto block: new_blocks)
    {
        blocks_inv.inventories.push_back({
            inventory_type_id::block, hash_block_header(block->header)});
    }
    auto ignore_handler = [](const std::error_code&, size_t) {};
    protocol_.broadcast(blocks_inv, ignore_handler);
}

void session::inventory(const std::error_code& ec,
    const inventory_type& packet, network::channel_ptr node)
{
    if (ec)
    {
        log_warning(LOG_SESSION) << "inventory: " << ec.message();
        return;
    }
    for (const inventory_vector_type& ivec: packet.inventories)
    {
        if (ivec.type == inventory_type_id::transaction)
            strand_.post(
                std::bind(&session::new_tx_inventory,
                    this, ivec.hash, node));
        else if (ivec.type == inventory_type_id::block);
            // Do nothing. Handled by poller.
        else
            log_warning(LOG_SESSION) << "Ignoring unknown inventory type";
    }
    node->subscribe_inventory(
        std::bind(&session::inventory, this, _1, _2, node));
}

void session::new_tx_inventory(const hash_digest& tx_hash, 
    network::channel_ptr node)
{
    if (grabbed_invs_.exists(tx_hash))
        return;
    log_debug(LOG_SESSION)
        << "Transaction inventory: " << encode_hex(tx_hash);
    // does it exist already
    // if not then issue getdata
    tx_pool_.exists(tx_hash,
        std::bind(&session::request_tx_data,
            this, _1, tx_hash, node));
    grabbed_invs_.store(tx_hash);
}

void session::get_blocks(const std::error_code& ec,
    const get_blocks_type& /* packet */, network::channel_ptr node)
{
    if (ec)
    {
        log_warning(LOG_SESSION) << "get_blocks: " << ec.message();
        return;
    }
    // TODO: Implement.
    // send 500 invs from last fork point
    // have memory of last inv, ready to trigger send next 500 once
    // getdata done for it.
    node->subscribe_get_blocks(
        std::bind(&session::get_blocks, this, _1, _2, node));
}

void handle_send_get_data(const std::error_code& ec)
{
    if (ec)
        log_error(LOG_SESSION) << "Requesting data: " << ec.message();
}
void session::request_tx_data(bool tx_exists,
    const hash_digest& tx_hash, network::channel_ptr node)
{
    if (tx_exists)
        return;
    get_data_type request_tx;
    request_tx.inventories.push_back(
        {inventory_type_id::transaction, tx_hash});
    node->send(request_tx, handle_send_get_data);
}

} // namespace node
} // namespace libbitcoin
