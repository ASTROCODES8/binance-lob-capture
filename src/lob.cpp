#include "lob.hpp"
#include <iostream>

namespace lob {

void OrderBook::apply_diff(const std::vector<LevelUpdate>& bids, const std::vector<LevelUpdate>& asks) {
    for (const auto& update : bids) {
        if (update.qty == 0) {
            bids_.erase(update.price);
        } else {
            bids_[update.price] = update.qty;
        }
    }

    for (const auto& update : asks) {
        if (update.qty == 0) {
            asks_.erase(update.price);
        } else {
            asks_[update.price] = update.qty;
        }
    }
}

void OrderBook::replace_top5_from_depth5(const std::vector<LevelUpdate>& depth5_bids,
                                        const std::vector<LevelUpdate>& depth5_asks) {
    if (!depth5_bids.empty()) {
        const int64_t worst_bid = depth5_bids.back().price;
        // Erase all levels at-or-better than the worst depth5 bid (price >= worst_bid)
        for (auto it = bids_.begin(); it != bids_.end(); ) {
            if (it->first >= worst_bid) {
                it = bids_.erase(it);
            } else {
                break;
            }
        }
        for (const auto& u : depth5_bids) {
            if (u.qty > 0) {
                bids_[u.price] = u.qty;
            }
        }
    }

    if (!depth5_asks.empty()) {
        const int64_t worst_ask = depth5_asks.back().price;
        // Erase all levels at-or-better than the worst depth5 ask (price <= worst_ask)
        for (auto it = asks_.begin(); it != asks_.end(); ) {
            if (it->first <= worst_ask) {
                it = asks_.erase(it);
            } else {
                break;
            }
        }
        for (const auto& u : depth5_asks) {
            if (u.qty > 0) {
                asks_[u.price] = u.qty;
            }
        }
    }

    is_anchored_ = true;
}

BookSnapshot OrderBook::top5() const noexcept {
    BookSnapshot snap{};

    size_t i = 0;
    for (auto it = bids_.begin(); it != bids_.end() && i < 5; ++it, ++i) {
        snap.bid_prices[i] = it->first;
        snap.bid_sizes[i] = it->second;
    }

    size_t j = 0;
    for (auto it = asks_.begin(); it != asks_.end() && j < 5; ++it, ++j) {
        snap.ask_prices[j] = it->first;
        snap.ask_sizes[j] = it->second;
    }

    return snap;
}

JsonPayloadParser::JsonPayloadParser() {
    padded_buffer_.reserve(65536);
}

bool JsonPayloadParser::parse_diff(std::string_view data_slice, DiffPayload& out) {
    out.bids.clear();
    out.asks.clear();

    padded_buffer_.assign(data_slice.data(), data_slice.size());
    padded_buffer_.append(simdjson::SIMDJSON_PADDING, ' ');

    simdjson::dom::element doc;
    auto error = parser_.parse(padded_buffer_.data(), data_slice.size()).get(doc);
    if (error) {
        return false;
    }

    auto u_val = doc["u"].get_uint64();
    if (!u_val.error()) out.u = u_val.value();

    auto U_val = doc["U"].get_uint64();
    if (!U_val.error()) out.U = U_val.value();

    out.pu = 0;
    auto pu_val = doc["pu"].get_uint64();
    if (!pu_val.error()) out.pu = pu_val.value();

    auto E_val = doc["E"].get_uint64();
    if (!E_val.error()) out.E = E_val.value();

    simdjson::dom::array bids_arr;
    if (!doc["b"].get_array().get(bids_arr)) {
        for (simdjson::dom::element item : bids_arr) {
            simdjson::dom::array pair;
            if (!item.get_array().get(pair)) {
                std::string_view p_str, q_str;
                if (!pair.at(0).get_string().get(p_str) && !pair.at(1).get_string().get(q_str)) {
                    LevelUpdate lu;
                    if (parse_scaled(p_str, lu.price) && parse_scaled(q_str, lu.qty)) {
                        out.bids.push_back(lu);
                    }
                }
            }
        }
    }

    simdjson::dom::array asks_arr;
    if (!doc["a"].get_array().get(asks_arr)) {
        for (simdjson::dom::element item : asks_arr) {
            simdjson::dom::array pair;
            if (!item.get_array().get(pair)) {
                std::string_view p_str, q_str;
                if (!pair.at(0).get_string().get(p_str) && !pair.at(1).get_string().get(q_str)) {
                    LevelUpdate lu;
                    if (parse_scaled(p_str, lu.price) && parse_scaled(q_str, lu.qty)) {
                        out.asks.push_back(lu);
                    }
                }
            }
        }
    }

    return true;
}

bool JsonPayloadParser::parse_depth5(std::string_view data_slice, Depth5Payload& out) {
    out.bids.clear();
    out.asks.clear();

    padded_buffer_.assign(data_slice.data(), data_slice.size());
    padded_buffer_.append(simdjson::SIMDJSON_PADDING, ' ');

    simdjson::dom::element doc;
    auto error = parser_.parse(padded_buffer_.data(), data_slice.size()).get(doc);
    if (error) {
        return false;
    }

    out.last_update_id = 0;
    auto luid_val = doc["lastUpdateId"].get_uint64();
    if (!luid_val.error()) {
        out.last_update_id = luid_val.value();
    } else {
        auto u_val = doc["u"].get_uint64();
        if (!u_val.error()) out.last_update_id = u_val.value();
    }

    simdjson::dom::array bids_arr;
    bool has_bids = (!doc["bids"].get_array().get(bids_arr)) || (!doc["b"].get_array().get(bids_arr));
    if (has_bids) {
        for (simdjson::dom::element item : bids_arr) {
            simdjson::dom::array pair;
            if (!item.get_array().get(pair)) {
                std::string_view p_str, q_str;
                if (!pair.at(0).get_string().get(p_str) && !pair.at(1).get_string().get(q_str)) {
                    LevelUpdate lu;
                    if (parse_scaled(p_str, lu.price) && parse_scaled(q_str, lu.qty)) {
                        out.bids.push_back(lu);
                    }
                }
            }
        }
    }

    simdjson::dom::array asks_arr;
    bool has_asks = (!doc["asks"].get_array().get(asks_arr)) || (!doc["a"].get_array().get(asks_arr));
    if (has_asks) {
        for (simdjson::dom::element item : asks_arr) {
            simdjson::dom::array pair;
            if (!item.get_array().get(pair)) {
                std::string_view p_str, q_str;
                if (!pair.at(0).get_string().get(p_str) && !pair.at(1).get_string().get(q_str)) {
                    LevelUpdate lu;
                    if (parse_scaled(p_str, lu.price) && parse_scaled(q_str, lu.qty)) {
                        out.asks.push_back(lu);
                    }
                }
            }
        }
    }

    return true;
}

bool JsonPayloadParser::parse_trade(std::string_view data_slice, TradePayload& out) {
    padded_buffer_.assign(data_slice.data(), data_slice.size());
    padded_buffer_.append(simdjson::SIMDJSON_PADDING, ' ');

    simdjson::dom::element doc;
    auto error = parser_.parse(padded_buffer_.data(), data_slice.size()).get(doc);
    if (error) {
        return false;
    }

    std::string_view p_str, q_str;
    if (!doc["p"].get_string().get(p_str)) {
        parse_scaled(p_str, out.price);
    }
    if (!doc["q"].get_string().get(q_str)) {
        parse_scaled(q_str, out.qty);
    }

    auto m_val = doc["m"].get_bool();
    if (!m_val.error()) out.is_buyer_maker = m_val.value();

    auto t_val = doc["t"].get_uint64();
    if (!t_val.error()) out.trade_id = t_val.value();

    auto T_val = doc["T"].get_uint64();
    if (!T_val.error()) out.trade_time = T_val.value();

    return true;
}

} // namespace lob
