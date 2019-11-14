// Copyright (c) 2012-2018, The CryptoNote developers, The Bytecoin developers.
// Licensed under the GNU Lesser General Public License. See LICENSE for details.

#include "Node.hpp"
#include <boost/algorithm/string.hpp>
#include <iostream>
#include "Config.hpp"
#include "CryptoNoteTools.hpp"
#include "TransactionExtra.hpp"
#include "common/JsonValue.hpp"
#include "platform/PathTools.hpp"
#include "platform/Time.hpp"
#include "seria/BinaryInputStream.hpp"
#include "seria/BinaryOutputStream.hpp"
#include "seria/KVBinaryInputStream.hpp"
#include "seria/KVBinaryOutputStream.hpp"
#include "version.hpp"

using namespace bytecoin;

Node::Node(logging::ILogger &log, const Config &config, BlockChainState &block_chain)
    : m_block_chain(block_chain)
    , m_config(config)
    , m_block_chain_was_far_behind(true)
    , m_log(log, "Node")
    , m_peer_db(config)
    , m_p2p(log, config, m_peer_db, std::bind(&Node::client_factory, this, _1, _2))
    , m_start_time(m_p2p.get_local_time())
    , m_commit_timer(std::bind(&Node::db_commit, this))
    , m_downloader(this, block_chain) {
	const std::string old_path = platform::get_default_data_directory(config.crypto_note_name);
	const std::string new_path = config.get_data_folder();

	if (!config.is_testnet) {
		m_block_chain_reader1 =
		    std::make_unique<LegacyBlockChainReader>(new_path + "/blockindexes.bin", new_path + "/blocks.bin");
		if (m_block_chain_reader1->get_block_count() <= block_chain.get_tip_height())
			m_block_chain_reader1.reset();
		if (new_path != old_path) {  // Current situation on Linux
			m_block_chain_reader2 =
			    std::make_unique<LegacyBlockChainReader>(old_path + "/blockindexes.bin", old_path + "/blocks.bin");
			if (m_block_chain_reader2->get_block_count() <= block_chain.get_tip_height())
				m_block_chain_reader2.reset();
		}
	}
	if (!config.bytecoind_bind_ip.empty() && config.bytecoind_bind_port != 0)
		m_api.reset(new http::Server(config.bytecoind_bind_ip, config.bytecoind_bind_port,
		    std::bind(&Node::on_api_http_request, this, _1, _2, _3), std::bind(&Node::on_api_http_disconnect, this, _1),
		    config.ssl_certificate_pem_file,
		    config.ssl_certificate_password ? config.ssl_certificate_password.get() : std::string()));

	m_commit_timer.once(DB_COMMIT_PERIOD_BYTECOIND);
	advance_long_poll();
}

bool Node::on_idle() {
	if (!m_block_chain_reader1 && !m_block_chain_reader2 &&
	    m_block_chain.get_tip_height() >= m_block_chain.internal_import_known_height())
		return m_downloader.on_idle();
	if (m_block_chain.get_tip_height() < m_block_chain.internal_import_known_height())
		m_block_chain.internal_import();
	else {
		if (m_block_chain_reader1 && !m_block_chain_reader1->import_blocks(&m_block_chain)) {
			m_block_chain_reader1.reset();
		}
		if (m_block_chain_reader2 && !m_block_chain_reader2->import_blocks(&m_block_chain)) {
			m_block_chain_reader2.reset();
		}
	}
	advance_long_poll();
	m_downloader.advance_download();
	return true;
}

void Node::sync_transactions(P2PClientBytecoin *who) {
	NOTIFY_REQUEST_TX_POOL::request msg;
	auto mytxs = m_block_chain.get_memory_state_transactions();
	msg.txs.reserve(mytxs.size());
	for (auto &&tx : mytxs) {
		msg.txs.push_back(tx.first);
	}
	BinaryArray raw_msg = LevinProtocol::send_message(NOTIFY_REQUEST_TX_POOL::ID, LevinProtocol::encode(msg), false);
	who->send(std::move(raw_msg));
}

void Node::P2PClientBytecoin::on_msg_bytes(size_t, size_t) {  // downloaded. uploaded
	//    node->peers.on_peer_bytes(get_address(), downloaded, uploaded,
	//    node->p2p.get_local_time());
}

CORE_SYNC_DATA
Node::P2PClientBytecoin::get_sync_data() const {
	CORE_SYNC_DATA sync_data;
	sync_data.current_height = m_node->m_block_chain.get_tip_height();
	sync_data.top_id         = m_node->m_block_chain.get_tip_bid();
	return sync_data;
}

std::vector<PeerlistEntry> Node::P2PClientBytecoin::get_peers_to_share() const {
	auto result = m_node->m_peer_db.get_peerlist_to_p2p(
	    get_address(), m_node->m_p2p.get_local_time(), config.p2p_default_peers_in_handshake);
	return result;
}

void Node::P2PClientBytecoin::on_first_message_after_handshake() {
	// if we set just seen on handshake, we will keep connecting to seed nodes
	// forever
	m_node->m_peer_db.set_peer_just_seen(
	    get_last_received_unique_number(), get_address(), m_node->m_p2p.get_local_time());
}

void Node::P2PClientBytecoin::after_handshake() {
	m_node->m_p2p.peers_updated();
	m_node->m_downloader.on_connect(this);
	m_node->advance_long_poll();

	auto signed_checkpoints = m_node->m_block_chain.get_latest_checkpoints();
	for (const auto & sck : signed_checkpoints) {
		BinaryArray raw_msg = LevinProtocol::send_message(NOTIFY_CHECKPOINT::ID, LevinProtocol::encode(sck), false);
		send(std::move(raw_msg));
	}
}

void Node::P2PClientBytecoin::on_msg_handshake(COMMAND_HANDSHAKE::request &&req) {
	m_node->m_peer_db.add_incoming_peer(get_address(), req.node_data.peer_id, m_node->m_p2p.get_local_time());
	after_handshake();
}

void Node::P2PClientBytecoin::on_msg_handshake(COMMAND_HANDSHAKE::response &&req) {
	m_node->m_peer_db.merge_peerlist_from_p2p(req.local_peerlist, m_node->m_p2p.get_local_time());
	after_handshake();
}

void Node::P2PClientBytecoin::on_msg_notify_request_chain(NOTIFY_REQUEST_CHAIN::request &&req) {
	NOTIFY_RESPONSE_CHAIN_ENTRY::request msg;
	msg.m_block_ids = m_node->m_block_chain.get_sync_headers_chain(
	    req.block_ids, &msg.start_height, config.p2p_block_ids_sync_default_count);
	msg.total_height = m_node->m_block_chain.get_tip_height() + 1;

	BinaryArray raw_msg =
	    LevinProtocol::send_message(NOTIFY_RESPONSE_CHAIN_ENTRY::ID, LevinProtocol::encode(msg), false);
	send(std::move(raw_msg));
}

void Node::P2PClientBytecoin::on_msg_notify_request_chain(NOTIFY_RESPONSE_CHAIN_ENTRY::request &&req) {
	m_node->m_downloader.on_msg_notify_request_chain(this, req);
}

void Node::P2PClientBytecoin::on_msg_notify_request_objects(NOTIFY_REQUEST_GET_OBJECTS::request &&req) {
	NOTIFY_RESPONSE_GET_OBJECTS::request msg;
	msg.current_blockchain_height = m_node->m_block_chain.get_tip_height() + 1;
	for (auto &&bh : req.blocks) {
		RawBlock raw_block;
		if (m_node->m_block_chain.read_block(bh, &raw_block)) {
			msg.blocks.push_back(RawBlockLegacy{raw_block.block, raw_block.transactions});
		} else
			msg.missed_ids.push_back(bh);
	}
	if (!req.txs.empty()) {
		// TODO - remove after we are sure transactions are never asked
		throw std::runtime_error(
		    "Transactions asked in NOTIFY_REQUEST_GET_OBJECTS by " + common::ip_address_to_string(get_address().ip));
	}
	BinaryArray raw_msg =
	    LevinProtocol::send_message(NOTIFY_RESPONSE_GET_OBJECTS::ID, LevinProtocol::encode(msg), false);
	send(std::move(raw_msg));
}

void Node::P2PClientBytecoin::on_msg_notify_request_objects(NOTIFY_RESPONSE_GET_OBJECTS::request &&req) {
	m_node->m_downloader.on_msg_notify_request_objects(this, req);
}

void Node::P2PClientBytecoin::on_disconnect(const std::string &ban_reason) {
	m_node->m_downloader.on_disconnect(this);

	P2PClientBasic::on_disconnect(ban_reason);
	m_node->advance_long_poll();
}

void Node::P2PClientBytecoin::on_msg_notify_request_tx_pool(NOTIFY_REQUEST_TX_POOL::request &&req) {
	NOTIFY_NEW_TRANSACTIONS::request msg;
	auto mytxs = m_node->m_block_chain.get_memory_state_transactions();
	msg.txs.reserve(mytxs.size());
	std::sort(req.txs.begin(), req.txs.end());  // Should have been sorted on wire,
	                                            // checked here, but alas, legacy
	for (auto &&tx : mytxs) {
		auto it = std::lower_bound(req.txs.begin(), req.txs.end(), tx.first);
		if (it != req.txs.end() && *it == tx.first)
			continue;
		msg.txs.push_back(tx.second.binary_tx);
	}
	m_node->m_log(logging::TRACE) << "on_msg_notify_request_tx_pool from " << get_address()
	                              << " peer sent=" << req.txs.size() << " we are relaying=" << msg.txs.size()
	                              << std::endl;
	if (msg.txs.empty())
		return;
	BinaryArray raw_msg = LevinProtocol::send_message(NOTIFY_NEW_TRANSACTIONS::ID, LevinProtocol::encode(msg), false);
	send(std::move(raw_msg));
}

void Node::P2PClientBytecoin::on_msg_timed_sync(COMMAND_TIMED_SYNC::request &&req) {
	m_node->m_downloader.advance_download();
}
void Node::P2PClientBytecoin::on_msg_timed_sync(COMMAND_TIMED_SYNC::response &&req) {
	m_node->m_downloader.advance_download();
}

void Node::P2PClientBytecoin::on_msg_notify_new_block(NOTIFY_NEW_BLOCK::request &&req) {
	RawBlock raw_block{req.b.block, req.b.transactions};
	PreparedBlock pb(std::move(raw_block), nullptr);
	api::BlockHeader info;
	auto action = m_node->m_block_chain.add_block(
	    pb, &info, common::ip_address_and_port_to_string(get_address().ip, get_address().port));
	switch (action) {
	case BroadcastAction::BAN:
		disconnect("NOTIFY_NEW_BLOCK add_block BAN");
		return;
	case BroadcastAction::BROADCAST_ALL: {
		req.hop += 1;
		BinaryArray raw_msg = LevinProtocol::send_message(NOTIFY_NEW_BLOCK::ID, LevinProtocol::encode(req), false);
		m_node->m_p2p.broadcast(this, raw_msg);

		m_node->advance_long_poll();
		break;
	}
	case BroadcastAction::NOTHING:
		break;
	}
	set_last_received_sync_data(CORE_SYNC_DATA{req.current_blockchain_height - 1, pb.bid});
	// -1 is in legacy protocol
	m_node->m_downloader.advance_download();
}

void Node::P2PClientBytecoin::on_msg_notify_new_transactions(NOTIFY_NEW_TRANSACTIONS::request &&req) {
	if (m_node->m_block_chain_reader1 || m_node->m_block_chain_reader2 ||
	    m_node->m_block_chain.get_tip_height() < m_node->m_block_chain.internal_import_known_height())
		return;  // We cannot check tx while downloading anyway
	NOTIFY_NEW_TRANSACTIONS::request msg;
	Hash any_tid;
	for (auto &&raw_tx : req.txs) {
		Transaction tx;
		try {
			seria::from_binary(tx, raw_tx);
		} catch (const std::exception &ex) {
			disconnect("NOTIFY_NEW_TRANSACTIONS add_transaction BAN from_binary failed " + std::string(ex.what()));
			return;
		}
		const Hash tid         = get_transaction_hash(tx);
		any_tid                = tid;
		Height conflict_height = 0;
		auto action            = m_node->m_block_chain.add_transaction(tid, tx, raw_tx, m_node->m_p2p.get_local_time(),
		    &conflict_height, common::ip_address_and_port_to_string(get_address().ip, get_address().port));
		switch (action) {
		case AddTransactionResult::BAN:
			disconnect("NOTIFY_NEW_TRANSACTIONS add_transaction BAN");
			return;
		case AddTransactionResult::BROADCAST_ALL:
			msg.txs.push_back(raw_tx);
			break;
		case AddTransactionResult::ALREADY_IN_POOL:
		case AddTransactionResult::INCREASE_FEE:
		case AddTransactionResult::FAILED_TO_REDO:
		case AddTransactionResult::OUTPUT_ALREADY_SPENT:
        case AddTransactionResult::TOO_OLD:
			break;
		}
	}
	m_node->m_log(logging::TRACE) << "on_msg_notify_new_transactions from " << get_address()
	                              << " got=" << req.txs.size() << " relaying=" << msg.txs.size()
	                              << (req.txs.size() > 1 ? " notify_tx_reply (?) " : " ")
	                              << (any_tid == Hash{} ? "" : common::pod_to_hex(any_tid)) << std::endl;
	if (msg.txs.empty())
		return;
	BinaryArray raw_msg = LevinProtocol::send_message(NOTIFY_NEW_TRANSACTIONS::ID, LevinProtocol::encode(msg), false);
	m_node->m_p2p.broadcast(this, raw_msg);
	m_node->advance_long_poll();
}
void Node::P2PClientBytecoin::on_msg_notify_checkpoint(NOTIFY_CHECKPOINT::request &&req) {
	if (!m_node->m_block_chain.add_checkpoint(
	        req, common::ip_address_and_port_to_string(get_address().ip, get_address().port)))
		return;
	m_node->m_log(logging::INFO) << "NOTIFY_CHECKPOINT::request height=" << req.height << " hash=" << req.hash
	                             << std::endl;
	BinaryArray raw_msg = LevinProtocol::send_message(NOTIFY_CHECKPOINT::ID, LevinProtocol::encode(req), false);
	m_node->m_p2p.broadcast(nullptr, raw_msg);  // nullptr, not this - so a sender sees "reflection" of message
    COMMAND_TIMED_SYNC::request ts_req;
        ts_req.payload_data = CORE_SYNC_DATA{m_node->m_block_chain.get_tip_height(), m_node->m_block_chain.get_tip_bid()};
        raw_msg = LevinProtocol::send_message(COMMAND_TIMED_SYNC::ID, LevinProtocol::encode(ts_req), true);
        m_node->m_p2p.broadcast(nullptr, raw_msg);
	m_node->advance_long_poll();
}

#if bytecoin_ALLOW_DEBUG_COMMANDS
void Node::P2PClientBytecoin::on_msg_network_state(COMMAND_REQUEST_NETWORK_STATE::request &&req) {
	if (!m_node->check_trust(req.tr)) {
		disconnect(std::string());
		return;
	}
	COMMAND_REQUEST_NETWORK_STATE::response msg;
	msg.local_time = m_node->m_p2p.get_local_time();
	msg.my_id      = get_unique_number();
	for (auto &&cc : m_node->m_downloader.get_good_clients()) {
		connection_entry item;
		item.is_income = cc.first->is_incoming();
		item.id        = cc.first->get_unique_number();
		item.adr       = cc.first->get_address();
		msg.connections_list.push_back(item);
	}
	BinaryArray raw_msg = LevinProtocol::send_reply(COMMAND_REQUEST_NETWORK_STATE::ID, LevinProtocol::encode(msg), 0);
	send(std::move(raw_msg));
}

void Node::P2PClientBytecoin::on_msg_stat_info(COMMAND_REQUEST_STAT_INFO::request &&req) {
	if (!m_node->check_trust(req.tr)) {
		disconnect(std::string());
		return;
	}
	COMMAND_REQUEST_STAT_INFO::response msg;
	msg.incoming_connections_count = m_node->m_p2p.good_clients(true).size();
	msg.connections_count          = msg.incoming_connections_count + m_node->m_p2p.good_clients(false).size();
	msg.version                    = app_version();
	msg.os_version                 = platform::get_os_version_string();
	msg.payload_info               = CoreStatistics{};
	BinaryArray raw_msg = LevinProtocol::send_reply(COMMAND_REQUEST_STAT_INFO::ID, LevinProtocol::encode(msg), 0);
	send(std::move(raw_msg));
}

#endif

bool Node::check_trust(const proof_of_trust &tr) {
	uint64_t local_time  = platform::now_unix_timestamp();
	uint64_t time_delata = local_time > tr.time ? local_time - tr.time : tr.time - local_time;

	if (time_delata > 24 * 60 * 60)
		return false;
	if (m_last_stat_request_time >= tr.time)
		return false;
	if (m_p2p.get_unique_number() != tr.peer_id)
		return false;

	crypto::Hash h = tr.get_hash();
	if (!crypto::check_signature(h, m_config.trusted_public_key, tr.sign))
		return false;
	m_last_stat_request_time = tr.time;
	return true;
}

void Node::advance_long_poll() {
	const auto now = m_p2p.get_local_time();
	if (!prevent_sleep && m_block_chain.get_tip().timestamp < now - 86400)
		prevent_sleep = std::make_unique<platform::PreventSleep>("Downloading blockchain");
	if (prevent_sleep &&
	    m_block_chain.get_tip().timestamp > now - m_block_chain.get_currency().block_future_time_limit * 2)
		prevent_sleep = nullptr;
	if (m_long_poll_http_clients.empty())
		return;
	api::bytecoind::GetStatus::Response resp = create_status_response3();
	json_rpc::Response last_json_resp;
	last_json_resp.set_result(resp);

	for (auto lit = m_long_poll_http_clients.begin(); lit != m_long_poll_http_clients.end();) {
		const bool method_status = lit->original_json_request.get_method() == api::bytecoind::GetStatus::method() ||
		                           lit->original_json_request.get_method() == api::bytecoind::GetStatus::method2();
		if (method_status && lit->original_get_status == resp) {
			++lit;
			continue;
		}
		if (!method_status && lit->original_get_status.top_block_hash == resp.top_block_hash &&
		    lit->original_get_status.transaction_pool_version == resp.transaction_pool_version) {
			++lit;
			continue;
		}
		http::ResponseData last_http_response;
		last_http_response.r.headers.push_back({"Content-Type", "application/json; charset=utf-8"});
		last_http_response.r.status             = 200;
		last_http_response.r.http_version_major = lit->original_request.r.http_version_major;
		last_http_response.r.http_version_minor = lit->original_request.r.http_version_minor;
		last_http_response.r.keep_alive         = lit->original_request.r.keep_alive;
		if (method_status) {
			last_json_resp.set_id(lit->original_json_request.get_id());
			last_http_response.set_body(last_json_resp.get_body());
		} else {
			json_rpc::Response gbt_json_resp;
			try {
				api::bytecoind::GetBlockTemplate::Request gbt_req;
				lit->original_json_request.load_params(gbt_req);
				api::bytecoind::GetBlockTemplate::Response gbt_res;
				getblocktemplate(std::move(gbt_req), gbt_res);
				gbt_json_resp.set_result(gbt_res);
				gbt_json_resp.set_id(lit->original_json_request.get_id());
			} catch (const json_rpc::Error &err) {
				gbt_json_resp.set_error(err);
			} catch (const std::exception &e) {
				gbt_json_resp.set_error(json_rpc::Error(json_rpc::INTERNAL_ERROR, e.what()));
			}
			last_http_response.set_body(gbt_json_resp.get_body());
		}
		lit->original_who->write(std::move(last_http_response));
		lit = m_long_poll_http_clients.erase(lit);
	}
}

static const std::string beautiful_index_start =
    R"(
<!DOCTYPE html>

<!-- ?v0.04 -->

<html>
<head lang="en">
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, minimum-scale=1">
    <title>GoldenDoge Block Explorer</title>
    <script src="//cdnjs.cloudflare.com/ajax/libs/jquery/2.1.1/jquery.min.js?v0.04"></script>
    <script src="//cdnjs.cloudflare.com/ajax/libs/jquery-timeago/1.4.0/jquery.timeago.min.js?v0.04"></script>
    <script src="//cdnjs.cloudflare.com/ajax/libs/jquery-sparklines/2.1.2/jquery.sparkline.min.js?v0.04"></script>
    <script src="//netdna.bootstrapcdn.com/bootstrap/3.1.1/js/bootstrap.min.js?v0.04"></script>
	<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/2.8.0/Chart.bundle.min.js"></script>
    <link href="//maxcdn.bootstrapcdn.com/font-awesome/4.7.0/css/font-awesome.min.css?v0.04" rel="stylesheet">
    <link href="//fonts.googleapis.com/css?family=Inconsolata" rel="stylesheet" type="text.css?v0.04">
    <script src="config.js"></script>
	<link href="css/themes/dark/style.css" rel="stylesheet">
</head>
<body>
<script>

	var blockchainExplorer 	= "/block#{id}";
	var transactionExplorer = "/tx#{id}";
	var paymentIdExplorer 	= "/payid#{id}";

	var style_cookie_name = "stylee";
	var style_cookie_duration = 365;
	var style_domain = window.location.hostname;

	$(function(){
		$('.theme-switch[rel="css/themes/dark/style.css"]').hide();
		set_style_from_cookie();

		$('.theme-switch').click(function() {
			swapStyleSheet($(this).attr('rel'));
			$('.theme-switch').show();
			$(this).hide();
			return false;
		});

		function swapStyleSheet(sheet){
			$('#theme_link').attr('href',sheet);
			$('.theme-switch').show();
			$('.theme-switch[rel="'+sheet+'"]').hide();
			set_cookie(style_cookie_name, sheet, style_cookie_duration, style_domain);
		}

		function set_style_from_cookie(){
			var style = get_cookie(style_cookie_name);
			if (style.length){
				swapStyleSheet(style);
			}
		}
		function set_cookie (cookie_name, cookie_value, lifespan_in_days, valid_domain){
			var domain_string = valid_domain ?
							   ("; domain=" + valid_domain) : '';
				document.cookie = cookie_name +
							   "=" + encodeURIComponent(cookie_value) +
							   "; max-age=" + 60 * 60 *
							   24 * lifespan_in_days +
							   "; path=/" + domain_string;
		}
		function get_cookie (cookie_name){
			var cookie_string = document.cookie;
			if (cookie_string.length != 0){
				var cookie_value = cookie_string.match(
							  '(^|;)[\s]*' +
							  cookie_name +
							  '=([^;]*)' );
				if(cookie_value != null && cookie_value.length>0) {
					return decodeURIComponent (cookie_value[2]);
				}
			}
			return '';
		}
	});

    function getTransactionUrl(id) {
        return transactionExplorer.replace('{symbol}', symbol.toLowerCase()).replace('{id}', id);
    }

    $.fn.update = function(txt){
        var el = this[0];
        if (el.textContent !== txt)
            el.textContent = txt;
        return this;
    };

    function updateTextClasses(className, text){
        var els = document.getElementsByClassName(className);
        for (var i = 0; i < els.length; i++){
            var el = els[i];
            if (el.textContent !== text)
                el.textContent = text;
        }
    }

    function updateText(elementId, text){
        var el = document.getElementById(elementId);
        if (el.textContent !== text){
            el.textContent = text;
        }
        return el;
    }

    function updateTextLinkable(elementId, text){
        var el = document.getElementById(elementId);
        if (el.innerHTML !== text){
            el.innerHTML = text;
        }
        return el;
    }

    var currentPage;
    var lastStats;
    var sync_blocks;
    var nodeStatus;

    function getReadableHashRateString(hashrate){
        var i = 0;
        var byteUnits = [' H', ' kH', ' MH', ' GH', ' TH', ' PH', ' EH', ' ZH', ' YH' ];
        while (hashrate > 1000){
            hashrate = hashrate / 1000;
            i++;
        }
        return hashrate.toFixed(2) + byteUnits[i];
    }

	function getReadableDifficultyString(difficulty, precision){
		if (isNaN(parseFloat(difficulty)) || !isFinite(difficulty)) return 0;
		if (typeof precision === 'undefined') precision = 2;
		var units = ['', 'k', 'M', 'G', 'T', 'P'],
            number = Math.floor(Math.log(difficulty) / Math.log(1000));
		if (units[number] === undefined || units[number] === null) {
            return 0
        }
        return (difficulty / Math.pow(1000, Math.floor(number))).toFixed(precision) + ' ' +  units[number];
    }

    function formatBlockLink(hash){
        return '<a href="' + getBlockchainUrl(hash) + '" target="_blank">' + hash + '</a>';
    }

    function getReadableCoins(coins, digits, withoutSymbol){
        var amount = (parseInt(coins || 0) / coinUnits).toFixed(digits || coinUnits.toString().length - 1);
        return amount + (withoutSymbol ? '' : (' ' + symbol));
    }

    function formatDate(time){
        if (!time) return '';
        return new Date(parseInt(time) * 1000).toLocaleString();
    }

	function formatBytes(a,b) {
		if(0==a)return"0 Bytes";var c=1024,d=b||2,e=["Bytes","KB","MB","GB","TB","PB","EB","ZB","YB"],f=Math.floor(Math.log(a)/Math.log(c));return parseFloat((a/Math.pow(c,f)).toFixed(d))+" "+e[f]
	}

	function formatPaymentLink(hash){
        return '<a href="' + getTransactionUrl(hash) + '" target="_blank">' + hash + '</a>';
    }

    function pulseLiveUpdate(){
        var stats_update = document.getElementById('stats_updated');
        stats_update.style.transition = 'opacity 100ms ease-out';
        stats_update.style.opacity = 1;
        setTimeout(function(){
            stats_update.style.transition = 'opacity 7000ms linear';
            stats_update.style.opacity = 0;
        }, 500);
    }

    window.onhashchange = function(){
        routePage();
    };


    function fetchLiveStats() {
        $.ajax({
            url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "",
                method:"get_status",
                params: {
                }
            }),
            dataType: 'json',
            cache: 'false'
        }).done(function(data, success){
            pulseLiveUpdate();
            lastStats = data;
			nodeStatus = success;
            currentPage.update();
			nodeInfo();
        }).always(function () {
			setTimeout(function() {
				fetchLiveStats();
			}, refreshDelay);
        });
    }

    function floatToString(float) {
        return float.toFixed(6).replace(/[0\.]+$/, '');
    }

	function nodeInfo() {
        if(nodeStatus) {

			$('#node_connection').html('Online').addClass('text-success').removeClass('text-danger');
			$('#node_height').html(parseInt(lastStats.result.top_block_height));
			$('#node_block').html(parseInt(lastStats.result.top_known_block_height));
			$('#node_diff').html(parseInt(lastStats.result.top_block_difficulty));
			//$('#node_alt').html(parseInt(lastStats['alt_blocks_count']));
			//$('#node_rpc').html(parseInt(lastStats['rpc_connections_count']));
			$('#node_inc').html(parseInt(lastStats.result.incoming_peer_count));
			$('#node_out').html(parseInt(lastStats.result.outgoing_peer_count));
			//$('#node_white').html(parseInt(lastStats['white_peerlist_size']));
			//$('#node_grey').html(parseInt(lastStats['grey_peerlist_size']));
			//if (lastStats['version'] !== 'undefined'){
				//$('#node_ver').html(lastStats.result.transaction_pool_version);
			//}
		} else {
			$('#node_connection').html('Offline').addClass('text-danger').removeClass('text-success');
		}
    }

    var xhrPageLoading;
    function routePage(loadedCallback) {

        if (currentPage) currentPage.destroy();
        $('#page').html('');
        $('#loading').show();

        if (xhrPageLoading)
            xhrPageLoading.abort();

        $('.hot_link').parent().removeClass('active');
        var $link = $('a.hot_link[href="' + (window.location.hash || '#') + '"]');

        $link.parent().addClass('active');
        var page = $link.data('page');

        xhrPageLoading = $.ajax({
            url: 'pages/' + page,
            cache: true,
            success: function (data) {
                $('#loading').hide();
                $('#page').show().html(data);
                currentPage.init();
                currentPage.update();
                if (loadedCallback) loadedCallback();
            }
        });
    }

    function getBlockchainUrl(id) {
        return blockchainExplorer.replace('{id}', id);
	}

	function getinfo() {
		$.ajax({
			url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "",
                method:"get_status",
                params: {
                }
            }),
            dataType: 'json',
            cache: 'false',
			timeout: 1500 //in milliseconds
		})
		.done(function (data) {
			try {
				lastStats = JSON.parse(data);
			} catch (e) {
				lastStats = data;
			}
			routePage(fetchLiveStats);
		})
		.fail(function () {
			//apiList.push(api);
			//api = apiList.shift();
			getinfo();
		});
	}



    $(function(){
		getinfo();
    });

    // Blockexplorer functions
    urlParam = function(name){
        var results = new RegExp('[\?&]' + name + '=([^&#]*)').exec(window.location.href);
        if (results==null){
           return null;
        }
        else{
           return results[1] || 0;
        }
    }

	$(function() {
        $('[data-toggle="tooltip"]').tooltip();
    });

	function hex2a(hexx) {
		var hex = hexx.toString();//force conversion
		var str = '';
		for (var i = 0; i < hex.length; i += 2)
			str += String.fromCharCode(parseInt(hex.substr(i, 2), 16));
		return str;
	}
</script>

<div class="navbar navbar-default navbar-fixed-top" role="navigation">
    <div class="container">
        <div class="navbar-header">
            <button type="button" class="navbar-toggle" data-toggle="collapse" data-target=".navbar-collapse">
                <span class="sr-only">Menu</span>
                <span class="icon-bar"></span>
                <span class="icon-bar"></span>
                <span class="icon-bar"></span>
            </button>
            <a class="navbar-brand " href=""><span id="notcoinIcon"><span style="color: #ffff00;">Đ</span></span> <strong><span style="color: #ffff00;">GoldenDoge</span></strong></a>
			<div id="stats_updated"><i class="fa fa-bolt"></i></div>
        </div>

        <div class="collapse navbar-collapse">

            <ul class="nav navbar-nav navbar-left explorer_menu">

			    <li><a class="hot_link" data-page="home.html" href="#">
                    <i class="fa fa-cubes" aria-hidden="true"></i> Explorer
                </a></li>

				<!--
				<li><a class="hot_link" data-page="check_payment.html" href="#check_payment">
                    <i class="fa fa-check-square-o" aria-hidden="true"></i> Check payment
                </a></li>
			
                <li>
				<button rel="css/themes/dark/style.css" class="btn btn-default theme-switch" data-toggle="tooltip" data-placement="bottom" title="" data-original-title="Switch to Night Mode"><i class="fa fa-moon-o"></i></button>
                </li>

                <li>
				<button rel="css/themes/white/style.css" class="btn btn-default theme-switch" data-toggle="tooltip" data-placement="bottom" title="" data-original-title="Switch to Day Mode"><i class="fa fa-sun-o"></i></button>
                </li>-->

	<!-- //-->

                <li style="display:none;"><a class="hot_link" data-page="blockchain_block.html" href="#blockchain_block"><i class="fa fa-cubes"></i> Block
                </a></li>

                <li style="display:none;"><a class="hot_link" data-page="blockchain_transaction.html" href="#blockchain_transaction"><i class="fa fa-cubes"></i> Transaction
                </a></li>

				<li style="display:none;"><a class="hot_link" data-page="blockchain_payment_id.html" href="#blockchain_payment_id"><i class="fa fa-cubes"></i> Transactions by Payment ID
                </a></li>

<!-- //-->
            </ul>


			<div class="nav col-md-6 navbar-right explorer-search">
				<div class="input-group">
					<input class="form-control" placeholder="Search by block height / hash, transaction hash" id="txt_search">
					<span class="input-group-btn"><button class="btn btn-default" type="button" id="btn_search">
						<span><i class="fa fa-search"></i> Search</span>
					</button></span>
				</div>
			</div>



		</div>
	  </div>
</div>


<script>
$('#btn_search').click(function(e) {

var text = document.getElementById('txt_search').value;

function GetSearchBlockbyHeight(){

	var block, xhrGetSearchBlockbyHeight;
    if (xhrGetSearchBlockbyHeight) xhrGetSearchBlockbyHeight.abort();

			xhrGetSearchBlockbyHeight = $.ajax({
            url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "blockbyheight",
                method:"getblockheaderbyheight",
                params: {
                   height: parseInt(text) + 1
                }
            }),
            dataType: 'json',
            cache: 'false',
            success: function(data){
				if(data.result.block_header){
					block = data.result.block_header;
					window.location.href = getBlockchainUrl(block.hash);
				} else if(data.error) {
					wrongSearchAlert();
				}
            }
        });
}

function GetSearchBlock(){
var block, xhrGetSearchBlock;
	if (xhrGetSearchBlock) xhrGetSearchBlock.abort();
		xhrGetSearchBlock = $.ajax({
            url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "",
                method:"get_raw_block",
                params: {
                   hash: text
                }
            }),
            dataType: 'json',
            cache: 'false',
			success: function(data){
				if(data.result){
					block = data.result;
					sessionStorage.setItem('searchBlock', JSON.stringify(block));
					window.location.href = getBlockchainUrl(block.hash);
				} else if(data.error) {
					$.ajax({
						url: api + '/json_rpc',
						method: "POST",
						data: JSON.stringify({
							jsonrpc:"2.0",
							id: "test",
							method:"get_raw_transaction",
							params: {
								hash: text
							}
						}),
						dataType: 'json',
						cache: 'false',
						success: function(data){
							  if(data.result){
								sessionStorage.setItem('searchTransaction', JSON.stringify(data.result));
								window.location.href = transactionExplorer.replace('{id}', text);
							  } else if(data.error) {
								xhrGetTsx =  $.ajax({
									url: api + '/json_rpc',
									method: "POST",
									data: JSON.stringify({
										jsonrpc:"2.0",
										id: "test",
										method:"k_transactions_by_payment_id",
										params: {
											payment_id: text
										}
									}),
									dataType: 'json',
									cache: 'false',
									success: function(data){
										if(data.result){
											txsByPaymentId = data.result.transactions;
											sessionStorage.setItem('txsByPaymentId', JSON.stringify(txsByPaymentId));
											window.location.href = paymentIdExplorer.replace('{id}', text);
										} else if(data.error) {
											$('#page').after(
												'<div class="alert alert-warning alert-dismissable fade in" style="position: fixed; right: 50px; top: 50px;">'+
													'<button type="button" class="close" ' +
															'data-dismiss="alert" aria-hidden="true">' +
														'&times;' +
													'</button>' +
													'We could not find anything.' +
												 '</div>');
										}
									}
								});

							  }
						}
					});
				}
			}
		});
}

if ( text.length < 64 ) {
	GetSearchBlockbyHeight();
} else if ( text.length == 64 ) {
	GetSearchBlock();
} else {
	wrongSearchAlert();
}

e.preventDefault();

});

function wrongSearchAlert() {
	$('#page').after(
		'<div class="alert alert-danger alert-dismissable fade in" style="position: fixed; right: 50px; top: 50px;">'+
		'<button type="button" class="close" ' +
		'data-dismiss="alert" aria-hidden="true">' +
		'&times;' +
		'</button>' +
		'<strong>Wrong search query!</strong><br /> Please enter block height or hash, transaction hash, or payment id.' +
		'</div>');
}

$('#txt_search').keyup(function(e){
        if(e.keyCode === 13)
            $('#btn_search').click();
});
</script>

<div id="content">
	<div class="container">

		<div id="page"></div>

		<p id="loading" class="text-center"><i class="fa fa-circle-o-notch fa-spin"></i></p>

	</div>
</div>

<footer>
	<div class="container">
		<div class="row">
			<div class="col-lg-4 col-md-4 col-sm-6">
				<p>
					<small>
				        <strong>GoldenDoge</strong>
					</small>
				</p>

				<ul>
                    <li><a href="https://GoldenDoge.org/" target="_blank">GoldenDoge.org</a></li>
				</ul>

			</div>
			<div class="col-lg-4 col-md-4 col-sm-6">
				<p>
					<small>


					Based on <a target="_blank" href="https://github.com/zelerius/zelerius-explorer"><i class="fa fa-github"></i> Zelerius Blockchain Explorer</a>
					<br />
					<span class="text-muted">Partially based on <strong>cryptonote-universal-pool</strong><br />
					open sourced under the <a href="http://www.gnu.org/licenses/gpl-2.0.html" target="_blank">GPL</a></span>
					</small>
				</p>
			</div>
			<div class="col-lg-4 col-md-4 col-sm-6">

				<strong class="text-info">Node info</strong>

				<ul class="text-info">
					<li>Status: <span id="node_connection" class="text-danger">Offline</span></li>
					<!--<li>Version: <span id="node_ver">...</span></li>-->
					<li>Height: <span id="node_height">...</span></li>
					<li>Last block: <span id="node_block">...</span></li>
					<li>Difficulty: <span id="node_diff">...</span></li>
					<!--<li>Alt. blocks: <span id="node_alt">...</span></li>-->
					<!--<li>RPC connections: <span id="node_rpc">...</span></li>-->
					<li>Incoming P2P connections: <span id="node_inc">...</span></li>
					<li>Outgoing P2P connects: <span id="node_out">...</span></li>
					<!--<li>White peers: <span id="node_white">...</span></li>-->
					<!--<li>Grey peers: <span id="node_grey">...</span></li>-->
				</ul>

			</div>
		</div>
    </div>
</footer>
<a href="#" class="scrollup"><i class="fa fa-chevron-circle-up"></i></a>
	<script type="text/javascript">
			jQuery(function($) { $(document).ready(function() {
				$(window).scroll(function(){
					if ($(this).scrollTop() > 500) {
						$('.scrollup').fadeIn();
					} else {
						$('.scrollup').fadeOut();
					}
				});

				$('.scrollup').click(function(){
					$("html, body").animate({ scrollTop: 0 }, 600);
					return false;
				});

				$('.scrollup').css('opacity','0.3');

				$('.scrollup').hover(function(){
					$(this).stop().animate({opacity: 0.9}, 400);
				 }, function(){
					$(this).stop().animate({opacity: 0.3}, 400);
				});

			});});
	</script>
</body>
</html>
        <td>GoldenDoge Node &bull; version)";
static const std::string beautiful_index_finish = " ";

static const std::string block_hash_html =
    R"(
<!DOCTYPE html>

<!-- ?v0.04 -->

<html>
<head lang="en">
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, minimum-scale=1">
    <title>GoldenDoge Block Explorer</title>
    <script src="//cdnjs.cloudflare.com/ajax/libs/jquery/2.1.1/jquery.min.js?v0.04"></script>
    <script src="//cdnjs.cloudflare.com/ajax/libs/jquery-timeago/1.4.0/jquery.timeago.min.js?v0.04"></script>
    <script src="//cdnjs.cloudflare.com/ajax/libs/jquery-sparklines/2.1.2/jquery.sparkline.min.js?v0.04"></script>
    <script src="//netdna.bootstrapcdn.com/bootstrap/3.1.1/js/bootstrap.min.js?v0.04"></script>
	<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/2.8.0/Chart.bundle.min.js"></script>
    <link href="//maxcdn.bootstrapcdn.com/font-awesome/4.7.0/css/font-awesome.min.css?v0.04" rel="stylesheet">
    <link href="//fonts.googleapis.com/css?family=Inconsolata" rel="stylesheet" type="text.css?v0.04">
    <script src="../config.js"></script>
	<link href="../css/themes/dark/style.css" rel="stylesheet">
</head>
<body>
<script>

	var blockchainExplorer 	= "../block#{id}";
	var transactionExplorer = "../tx#{id}";
	var paymentIdExplorer 	= "../payid#{id}";

	var style_cookie_name = "stylee";
	var style_cookie_duration = 365;
	var style_domain = window.location.hostname;

	$(function(){
		$('.theme-switch[rel="../css/themes/dark/style.css"]').hide();
		set_style_from_cookie();

		$('.theme-switch').click(function() {
			swapStyleSheet($(this).attr('rel'));
			$('.theme-switch').show();
			$(this).hide();
			return false;
		});

		function swapStyleSheet(sheet){
			$('#theme_link').attr('href',sheet);
			$('.theme-switch').show();
			$('.theme-switch[rel="'+sheet+'"]').hide();
			set_cookie(style_cookie_name, sheet, style_cookie_duration, style_domain);
		}

		function set_style_from_cookie(){
			var style = get_cookie(style_cookie_name);
			if (style.length){
				swapStyleSheet(style);
			}
		}
		function set_cookie (cookie_name, cookie_value, lifespan_in_days, valid_domain){
			var domain_string = valid_domain ?
							   ("; domain=" + valid_domain) : '';
				document.cookie = cookie_name +
							   "=" + encodeURIComponent(cookie_value) +
							   "; max-age=" + 60 * 60 *
							   24 * lifespan_in_days +
							   "; path=/" + domain_string;
		}
		function get_cookie (cookie_name){
			var cookie_string = document.cookie;
			if (cookie_string.length != 0){
				var cookie_value = cookie_string.match(
							  '(^|;)[\s]*' +
							  cookie_name +
							  '=([^;]*)' );
				if(cookie_value != null && cookie_value.length>0) {
					return decodeURIComponent (cookie_value[2]);
				}
			}
			return '';
		}
	});

    function getTransactionUrl(id) {
        return transactionExplorer.replace('{symbol}', symbol.toLowerCase()).replace('{id}', id);
    }

    $.fn.update = function(txt){
        var el = this[0];
        if (el.textContent !== txt)
            el.textContent = txt;
        return this;
    };

    function updateTextClasses(className, text){
        var els = document.getElementsByClassName(className);
        for (var i = 0; i < els.length; i++){
            var el = els[i];
            if (el.textContent !== text)
                el.textContent = text;
        }
    }

    function updateText(elementId, text){
        var el = document.getElementById(elementId);
        if (el.textContent !== text){
            el.textContent = text;
        }
        return el;
    }

    function updateTextLinkable(elementId, text){
        var el = document.getElementById(elementId);
        if (el.innerHTML !== text){
            el.innerHTML = text;
        }
        return el;
    }

    var currentPage;
    var lastStats;
    var sync_blocks;
    var nodeStatus;

    function getReadableHashRateString(hashrate){
        var i = 0;
        var byteUnits = [' H', ' kH', ' MH', ' GH', ' TH', ' PH', ' EH', ' ZH', ' YH' ];
        while (hashrate > 1000){
            hashrate = hashrate / 1000;
            i++;
        }
        return hashrate.toFixed(2) + byteUnits[i];
    }

	function getReadableDifficultyString(difficulty, precision){
		if (isNaN(parseFloat(difficulty)) || !isFinite(difficulty)) return 0;
		if (typeof precision === 'undefined') precision = 2;
		var units = ['', 'k', 'M', 'G', 'T', 'P'],
            number = Math.floor(Math.log(difficulty) / Math.log(1000));
		if (units[number] === undefined || units[number] === null) {
            return 0
        }
        return (difficulty / Math.pow(1000, Math.floor(number))).toFixed(precision) + ' ' +  units[number];
    }

    function formatBlockLink(hash){
        return '<a href="' + getBlockchainUrl(hash) + '" target="_blank">' + hash + '</a>';
    }

    function getReadableCoins(coins, digits, withoutSymbol){
        var amount = (parseInt(coins || 0) / coinUnits).toFixed(digits || coinUnits.toString().length - 1);
        return amount + (withoutSymbol ? '' : (' ' + symbol));
    }

    function formatDate(time){
        if (!time) return '';
        return new Date(parseInt(time) * 1000).toLocaleString();
    }

	function formatBytes(a,b) {
		if(0==a)return"0 Bytes";var c=1024,d=b||2,e=["Bytes","KB","MB","GB","TB","PB","EB","ZB","YB"],f=Math.floor(Math.log(a)/Math.log(c));return parseFloat((a/Math.pow(c,f)).toFixed(d))+" "+e[f]
	}

	function formatPaymentLink(hash){
        return '<a href="' + getTransactionUrl(hash) + '" target="_blank">' + hash + '</a>';
    }

    function pulseLiveUpdate(){
        var stats_update = document.getElementById('stats_updated');
        stats_update.style.transition = 'opacity 100ms ease-out';
        stats_update.style.opacity = 1;
        setTimeout(function(){
            stats_update.style.transition = 'opacity 7000ms linear';
            stats_update.style.opacity = 0;
        }, 500);
    }

    window.onhashchange = function(){
        routePage();
    };


    function fetchLiveStats() {
        $.ajax({
            url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "",
                method:"get_status",
                params: {
                }
            }),
            dataType: 'json',
            cache: 'false'
        }).done(function(data, success){
            pulseLiveUpdate();
            lastStats = data;
			nodeStatus = success;
            currentPage.update();
			nodeInfo();
        }).always(function () {
			setTimeout(function() {
				fetchLiveStats();
			}, refreshDelay);
        });
    }

    function floatToString(float) {
        return float.toFixed(6).replace(/[0\.]+$/, '');
    }

	function nodeInfo() {
        if(nodeStatus) {

			$('#node_connection').html('Online').addClass('text-success').removeClass('text-danger');
			$('#node_height').html(parseInt(lastStats.result.top_block_height));
			$('#node_block').html(parseInt(lastStats.result.top_known_block_height));
			$('#node_diff').html(parseInt(lastStats.result.top_block_difficulty));
			//$('#node_alt').html(parseInt(lastStats['alt_blocks_count']));
			//$('#node_rpc').html(parseInt(lastStats['rpc_connections_count']));
			$('#node_inc').html(parseInt(lastStats.result.incoming_peer_count));
			$('#node_out').html(parseInt(lastStats.result.outgoing_peer_count));
			//$('#node_white').html(parseInt(lastStats['white_peerlist_size']));
			//$('#node_grey').html(parseInt(lastStats['grey_peerlist_size']));
			//if (lastStats['version'] !== 'undefined'){
				//$('#node_ver').html(lastStats.result.transaction_pool_version);
			//}
		} else {
			$('#node_connection').html('Offline').addClass('text-danger').removeClass('text-success');
		}
    }

    var xhrPageLoading;
    function routePage(loadedCallback) {

        if (currentPage) currentPage.destroy();
        $('#page').html('');
        $('#loading').show();

        if (xhrPageLoading)
            xhrPageLoading.abort();

        $('.hot_link').parent().removeClass('active');
        var $link = $('a.hot_link[href="' + (window.location.hash || '#') + '"]');

        $link.parent().addClass('active');
        var page = $link.data('page');

        xhrPageLoading = $.ajax({
            url: '../pages/' + 'blockchain_block.html',
            cache: true,
            success: function (data) {
                $('#loading').hide();
                $('#page').show().html(data);
                currentPage.init();
                currentPage.update();
                if (loadedCallback) loadedCallback();
            }
        });
    }

    function getBlockchainUrl(id) {
        return blockchainExplorer.replace('{id}', id);
	}

	function getinfo() {
		$.ajax({
			url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "",
                method:"get_status",
                params: {
                }
            }),
            dataType: 'json',
            cache: 'false',
			timeout: 1500 //in milliseconds
		})
		.done(function (data) {
			try {
				lastStats = JSON.parse(data);
			} catch (e) {
				lastStats = data;
			}
			routePage(fetchLiveStats);
		})
		.fail(function () {
			//apiList.push(api);
			//api = apiList.shift();
			getinfo();
		});
	}



    $(function(){
		getinfo();
    });

    // Blockexplorer functions
    urlParam = function(name){
        var results = new RegExp('[\?&]' + name + '=([^&#]*)').exec(window.location.href);
        if (results==null){
           return null;
        }
        else{
           return results[1] || 0;
        }
    }

	$(function() {
        $('[data-toggle="tooltip"]').tooltip();
    });

	function hex2a(hexx) {
		var hex = hexx.toString();//force conversion
		var str = '';
		for (var i = 0; i < hex.length; i += 2)
			str += String.fromCharCode(parseInt(hex.substr(i, 2), 16));
		return str;
	}
</script>

<div class="navbar navbar-default navbar-fixed-top" role="navigation">
    <div class="container">
        <div class="navbar-header">
            <button type="button" class="navbar-toggle" data-toggle="collapse" data-target=".navbar-collapse">
                <span class="sr-only">Menu</span>
                <span class="icon-bar"></span>
                <span class="icon-bar"></span>
                <span class="icon-bar"></span>
            </button>
            <a class="navbar-brand " href=".."><span id="notcoinIcon"><span style="color: #ffff00;">Đ</span></span> <strong><span style="color: #ffff00;">GoldenDoge</span></strong></a>
			<div id="stats_updated"><i class="fa fa-bolt"></i></div>
        </div>

        <div class="collapse navbar-collapse">

            <ul class="nav navbar-nav navbar-left explorer_menu">

			    <li><a class="hot_link" data-page="" href="..">
                    <i class="fa fa-cubes" aria-hidden="true"></i> Explorer
                </a></li>

				<!--
				<li><a class="hot_link" data-page="check_payment.html" href="#check_payment">
                    <i class="fa fa-check-square-o" aria-hidden="true"></i> Check payment
                </a></li>
			
                <li>
				<button rel="css/themes/dark/style.css" class="btn btn-default theme-switch" data-toggle="tooltip" data-placement="bottom" title="" data-original-title="Switch to Night Mode"><i class="fa fa-moon-o"></i></button>
                </li>

                <li>
				<button rel="css/themes/white/style.css" class="btn btn-default theme-switch" data-toggle="tooltip" data-placement="bottom" title="" data-original-title="Switch to Day Mode"><i class="fa fa-sun-o"></i></button>
                </li>-->

	<!-- //-->

                <li style="display:none;"><a class="hot_link" data-page="blockchain_block.html" href="#blockchain_block"><i class="fa fa-cubes"></i> Block
                </a></li>

                <li style="display:none;"><a class="hot_link" data-page="blockchain_transaction.html" href="#blockchain_transaction"><i class="fa fa-cubes"></i> Transaction
                </a></li>

				<li style="display:none;"><a class="hot_link" data-page="blockchain_payment_id.html" href="#blockchain_payment_id"><i class="fa fa-cubes"></i> Transactions by Payment ID
                </a></li>

<!-- //-->
            </ul>


			<div class="nav col-md-6 navbar-right explorer-search">
				<div class="input-group">
					<input class="form-control" placeholder="Search by block height / hash, transaction hash" id="txt_search">
					<span class="input-group-btn"><button class="btn btn-default" type="button" id="btn_search">
						<span><i class="fa fa-search"></i> Search</span>
					</button></span>
				</div>
			</div>



		</div>
	  </div>
</div>


<script>
$('#btn_search').click(function(e) {

var text = document.getElementById('txt_search').value;

function GetSearchBlockbyHeight(){

	var block, xhrGetSearchBlockbyHeight;
    if (xhrGetSearchBlockbyHeight) xhrGetSearchBlockbyHeight.abort();

			xhrGetSearchBlockbyHeight = $.ajax({
            url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "blockbyheight",
                method:"getblockheaderbyheight",
                params: {
                   height: parseInt(text) + 1
                }
            }),
            dataType: 'json',
            cache: 'false',
            success: function(data){
				if(data.result.block_header){
					block = data.result.block_header;
					window.location.href = getBlockchainUrl(block.hash);
				} else if(data.error) {
					wrongSearchAlert();
				}
            }
        });
}

function GetSearchBlock(){
var block, xhrGetSearchBlock;
	if (xhrGetSearchBlock) xhrGetSearchBlock.abort();
		xhrGetSearchBlock = $.ajax({
            url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "",
                method:"get_raw_block",
                params: {
                   hash: text
                }
            }),
            dataType: 'json',
            cache: 'false',
			success: function(data){
				if(data.result){
					block = data.result;
					sessionStorage.setItem('searchBlock', JSON.stringify(block));
					window.location.href = getBlockchainUrl(block.hash);
				} else if(data.error) {
					$.ajax({
						url: api + '/json_rpc',
						method: "POST",
						data: JSON.stringify({
							jsonrpc:"2.0",
							id: "test",
							method:"get_raw_transaction",
							params: {
								hash: text
							}
						}),
						dataType: 'json',
						cache: 'false',
						success: function(data){
							  if(data.result){
								sessionStorage.setItem('searchTransaction', JSON.stringify(data.result));
								window.location.href = transactionExplorer.replace('{id}', text);
							  } else if(data.error) {
								xhrGetTsx =  $.ajax({
									url: api + '/json_rpc',
									method: "POST",
									data: JSON.stringify({
										jsonrpc:"2.0",
										id: "test",
										method:"k_transactions_by_payment_id",
										params: {
											payment_id: text
										}
									}),
									dataType: 'json',
									cache: 'false',
									success: function(data){
										if(data.result){
											txsByPaymentId = data.result.transactions;
											sessionStorage.setItem('txsByPaymentId', JSON.stringify(txsByPaymentId));
											window.location.href = paymentIdExplorer.replace('{id}', text);
										} else if(data.error) {
											$('#page').after(
												'<div class="alert alert-warning alert-dismissable fade in" style="position: fixed; right: 50px; top: 50px;">'+
													'<button type="button" class="close" ' +
															'data-dismiss="alert" aria-hidden="true">' +
														'&times;' +
													'</button>' +
													'We could not find anything.' +
												 '</div>');
										}
									}
								});

							  }
						}
					});
				}
			}
		});
}

if ( text.length < 64 ) {
	GetSearchBlockbyHeight();
} else if ( text.length == 64 ) {
	GetSearchBlock();
} else {
	wrongSearchAlert();
}

e.preventDefault();

});

function wrongSearchAlert() {
	$('#page').after(
		'<div class="alert alert-danger alert-dismissable fade in" style="position: fixed; right: 50px; top: 50px;">'+
		'<button type="button" class="close" ' +
		'data-dismiss="alert" aria-hidden="true">' +
		'&times;' +
		'</button>' +
		'<strong>Wrong search query!</strong><br /> Please enter block height or hash, transaction hash, or payment id.' +
		'</div>');
}

$('#txt_search').keyup(function(e){
        if(e.keyCode === 13)
            $('#btn_search').click();
});
</script>

<div id="content">
	<div class="container">

		<div id="page"></div>

		<p id="loading" class="text-center"><i class="fa fa-circle-o-notch fa-spin"></i></p>

	</div>
</div>

<footer>
	<div class="container">
		<div class="row">
			<div class="col-lg-4 col-md-4 col-sm-6">
				<p>
					<small>
				        <strong>GoldenDoge</strong>
					</small>
				</p>

				<ul>
                    <li><a href="https://GoldenDoge.org/" target="_blank">GoldenDoge.org</a></li>
				</ul>

			</div>
			<div class="col-lg-4 col-md-4 col-sm-6">
				<p>
					<small>


					Based on <a target="_blank" href="https://github.com/zelerius/zelerius-explorer"><i class="fa fa-github"></i> Zelerius Blockchain Explorer</a>
					<br />
					<span class="text-muted">Partially based on <strong>cryptonote-universal-pool</strong><br />
					open sourced under the <a href="http://www.gnu.org/licenses/gpl-2.0.html" target="_blank">GPL</a></span>
					</small>
				</p>
			</div>
			<div class="col-lg-4 col-md-4 col-sm-6">

				<strong class="text-info">Node info</strong>

				<ul class="text-info">
					<li>Status: <span id="node_connection" class="text-danger">Offline</span></li>
					<!--<li>Version: <span id="node_ver">...</span></li>-->
					<li>Height: <span id="node_height">...</span></li>
					<li>Last block: <span id="node_block">...</span></li>
					<li>Difficulty: <span id="node_diff">...</span></li>
					<!--<li>Alt. blocks: <span id="node_alt">...</span></li>-->
					<!--<li>RPC connections: <span id="node_rpc">...</span></li>-->
					<li>Incoming P2P connections: <span id="node_inc">...</span></li>
					<li>Outgoing P2P connects: <span id="node_out">...</span></li>
					<!--<li>White peers: <span id="node_white">...</span></li>-->
					<!--<li>Grey peers: <span id="node_grey">...</span></li>-->
				</ul>

			</div>
		</div>
    </div>
</footer>
<a href="#" class="scrollup"><i class="fa fa-chevron-circle-up"></i></a>
	<script type="text/javascript">
			jQuery(function($) { $(document).ready(function() {
				$(window).scroll(function(){
					if ($(this).scrollTop() > 500) {
						$('.scrollup').fadeIn();
					} else {
						$('.scrollup').fadeOut();
					}
				});

				$('.scrollup').click(function(){
					$("html, body").animate({ scrollTop: 0 }, 600);
					return false;
				});

				$('.scrollup').css('opacity','0.3');

				$('.scrollup').hover(function(){
					$(this).stop().animate({opacity: 0.9}, 400);
				 }, function(){
					$(this).stop().animate({opacity: 0.3}, 400);
				});

			});});
	</script>
</body>
</html>
        <td>GoldenDoge Node &bull; version)";

static const std::string tx_hash_html =
    R"(
<!DOCTYPE html>

<!-- ?v0.04 -->

<html>
<head lang="en">
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, minimum-scale=1">
    <title>GoldenDoge Block Explorer</title>
    <script src="//cdnjs.cloudflare.com/ajax/libs/jquery/2.1.1/jquery.min.js?v0.04"></script>
    <script src="//cdnjs.cloudflare.com/ajax/libs/jquery-timeago/1.4.0/jquery.timeago.min.js?v0.04"></script>
    <script src="//cdnjs.cloudflare.com/ajax/libs/jquery-sparklines/2.1.2/jquery.sparkline.min.js?v0.04"></script>
    <script src="//netdna.bootstrapcdn.com/bootstrap/3.1.1/js/bootstrap.min.js?v0.04"></script>
	<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/2.8.0/Chart.bundle.min.js"></script>
    <link href="//maxcdn.bootstrapcdn.com/font-awesome/4.7.0/css/font-awesome.min.css?v0.04" rel="stylesheet">
    <link href="//fonts.googleapis.com/css?family=Inconsolata" rel="stylesheet" type="text.css?v0.04">
    <script src="../config.js"></script>
	<link href="../css/themes/dark/style.css" rel="stylesheet">
</head>
<body>
<script>

	var blockchainExplorer 	= "../block#{id}";
	var transactionExplorer = "../tx#{id}";
	var paymentIdExplorer 	= "../payid#{id}";

	var style_cookie_name = "stylee";
	var style_cookie_duration = 365;
	var style_domain = window.location.hostname;

	$(function(){
		$('.theme-switch[rel="../css/themes/dark/style.css"]').hide();
		set_style_from_cookie();

		$('.theme-switch').click(function() {
			swapStyleSheet($(this).attr('rel'));
			$('.theme-switch').show();
			$(this).hide();
			return false;
		});

		function swapStyleSheet(sheet){
			$('#theme_link').attr('href',sheet);
			$('.theme-switch').show();
			$('.theme-switch[rel="'+sheet+'"]').hide();
			set_cookie(style_cookie_name, sheet, style_cookie_duration, style_domain);
		}

		function set_style_from_cookie(){
			var style = get_cookie(style_cookie_name);
			if (style.length){
				swapStyleSheet(style);
			}
		}
		function set_cookie (cookie_name, cookie_value, lifespan_in_days, valid_domain){
			var domain_string = valid_domain ?
							   ("; domain=" + valid_domain) : '';
				document.cookie = cookie_name +
							   "=" + encodeURIComponent(cookie_value) +
							   "; max-age=" + 60 * 60 *
							   24 * lifespan_in_days +
							   "; path=/" + domain_string;
		}
		function get_cookie (cookie_name){
			var cookie_string = document.cookie;
			if (cookie_string.length != 0){
				var cookie_value = cookie_string.match(
							  '(^|;)[\s]*' +
							  cookie_name +
							  '=([^;]*)' );
				if(cookie_value != null && cookie_value.length>0) {
					return decodeURIComponent (cookie_value[2]);
				}
			}
			return '';
		}
	});

    function getTransactionUrl(id) {
        return transactionExplorer.replace('{symbol}', symbol.toLowerCase()).replace('{id}', id);
    }

    $.fn.update = function(txt){
        var el = this[0];
        if (el.textContent !== txt)
            el.textContent = txt;
        return this;
    };

    function updateTextClasses(className, text){
        var els = document.getElementsByClassName(className);
        for (var i = 0; i < els.length; i++){
            var el = els[i];
            if (el.textContent !== text)
                el.textContent = text;
        }
    }

    function updateText(elementId, text){
        var el = document.getElementById(elementId);
        if (el.textContent !== text){
            el.textContent = text;
        }
        return el;
    }

    function updateTextLinkable(elementId, text){
        var el = document.getElementById(elementId);
        if (el.innerHTML !== text){
            el.innerHTML = text;
        }
        return el;
    }

    var currentPage;
    var lastStats;
    var sync_blocks;
    var nodeStatus;

    function getReadableHashRateString(hashrate){
        var i = 0;
        var byteUnits = [' H', ' kH', ' MH', ' GH', ' TH', ' PH', ' EH', ' ZH', ' YH' ];
        while (hashrate > 1000){
            hashrate = hashrate / 1000;
            i++;
        }
        return hashrate.toFixed(2) + byteUnits[i];
    }

	function getReadableDifficultyString(difficulty, precision){
		if (isNaN(parseFloat(difficulty)) || !isFinite(difficulty)) return 0;
		if (typeof precision === 'undefined') precision = 2;
		var units = ['', 'k', 'M', 'G', 'T', 'P'],
            number = Math.floor(Math.log(difficulty) / Math.log(1000));
		if (units[number] === undefined || units[number] === null) {
            return 0
        }
        return (difficulty / Math.pow(1000, Math.floor(number))).toFixed(precision) + ' ' +  units[number];
    }

    function formatBlockLink(hash){
        return '<a href="' + getBlockchainUrl(hash) + '" target="_blank">' + hash + '</a>';
    }

    function getReadableCoins(coins, digits, withoutSymbol){
        var amount = (parseInt(coins || 0) / coinUnits).toFixed(digits || coinUnits.toString().length - 1);
        return amount + (withoutSymbol ? '' : (' ' + symbol));
    }

    function formatDate(time){
        if (!time) return '';
        return new Date(parseInt(time) * 1000).toLocaleString();
    }

	function formatBytes(a,b) {
		if(0==a)return"0 Bytes";var c=1024,d=b||2,e=["Bytes","KB","MB","GB","TB","PB","EB","ZB","YB"],f=Math.floor(Math.log(a)/Math.log(c));return parseFloat((a/Math.pow(c,f)).toFixed(d))+" "+e[f]
	}

	function formatPaymentLink(hash){
        return '<a href="' + getTransactionUrl(hash) + '" target="_blank">' + hash + '</a>';
    }

    function pulseLiveUpdate(){
        var stats_update = document.getElementById('stats_updated');
        stats_update.style.transition = 'opacity 100ms ease-out';
        stats_update.style.opacity = 1;
        setTimeout(function(){
            stats_update.style.transition = 'opacity 7000ms linear';
            stats_update.style.opacity = 0;
        }, 500);
    }

    window.onhashchange = function(){
        routePage();
    };


    function fetchLiveStats() {
        $.ajax({
            url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "",
                method:"get_status",
                params: {
                }
            }),
            dataType: 'json',
            cache: 'false'
        }).done(function(data, success){
            pulseLiveUpdate();
            lastStats = data;
			nodeStatus = success;
            currentPage.update();
			nodeInfo();
        }).always(function () {
			setTimeout(function() {
				fetchLiveStats();
			}, refreshDelay);
        });
    }

    function floatToString(float) {
        return float.toFixed(6).replace(/[0\.]+$/, '');
    }

	function nodeInfo() {
        if(nodeStatus) {

			$('#node_connection').html('Online').addClass('text-success').removeClass('text-danger');
			$('#node_height').html(parseInt(lastStats.result.top_block_height));
			$('#node_block').html(parseInt(lastStats.result.top_known_block_height));
			$('#node_diff').html(parseInt(lastStats.result.top_block_difficulty));
			//$('#node_alt').html(parseInt(lastStats['alt_blocks_count']));
			//$('#node_rpc').html(parseInt(lastStats['rpc_connections_count']));
			$('#node_inc').html(parseInt(lastStats.result.incoming_peer_count));
			$('#node_out').html(parseInt(lastStats.result.outgoing_peer_count));
			//$('#node_white').html(parseInt(lastStats['white_peerlist_size']));
			//$('#node_grey').html(parseInt(lastStats['grey_peerlist_size']));
			//if (lastStats['version'] !== 'undefined'){
				//$('#node_ver').html(lastStats.result.transaction_pool_version);
			//}
		} else {
			$('#node_connection').html('Offline').addClass('text-danger').removeClass('text-success');
		}
    }

    var xhrPageLoading;
    function routePage(loadedCallback) {

        if (currentPage) currentPage.destroy();
        $('#page').html('');
        $('#loading').show();

        if (xhrPageLoading)
            xhrPageLoading.abort();

        $('.hot_link').parent().removeClass('active');
        var $link = $('a.hot_link[href="' + (window.location.hash || '#') + '"]');

        $link.parent().addClass('active');
        var page = $link.data('page');

        xhrPageLoading = $.ajax({
            url: '../pages/' + 'blockchain_transaction.html',
            cache: true,
            success: function (data) {
                $('#loading').hide();
                $('#page').show().html(data);
                currentPage.init();
                currentPage.update();
                if (loadedCallback) loadedCallback();
            }
        });
    }

    function getBlockchainUrl(id) {
        return blockchainExplorer.replace('{id}', id);
	}

	function getinfo() {
		$.ajax({
			url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "",
                method:"get_status",
                params: {
                }
            }),
            dataType: 'json',
            cache: 'false',
			timeout: 1500 //in milliseconds
		})
		.done(function (data) {
			try {
				lastStats = JSON.parse(data);
			} catch (e) {
				lastStats = data;
			}
			routePage(fetchLiveStats);
		})
		.fail(function () {
			//apiList.push(api);
			//api = apiList.shift();
			getinfo();
		});
	}



    $(function(){
		getinfo();
    });

    // Blockexplorer functions
    urlParam = function(name){
        var results = new RegExp('[\?&]' + name + '=([^&#]*)').exec(window.location.href);
        if (results==null){
           return null;
        }
        else{
           return results[1] || 0;
        }
    }

	$(function() {
        $('[data-toggle="tooltip"]').tooltip();
    });

	function hex2a(hexx) {
		var hex = hexx.toString();//force conversion
		var str = '';
		for (var i = 0; i < hex.length; i += 2)
			str += String.fromCharCode(parseInt(hex.substr(i, 2), 16));
		return str;
	}
</script>

<div class="navbar navbar-default navbar-fixed-top" role="navigation">
    <div class="container">
        <div class="navbar-header">
            <button type="button" class="navbar-toggle" data-toggle="collapse" data-target=".navbar-collapse">
                <span class="sr-only">Menu</span>
                <span class="icon-bar"></span>
                <span class="icon-bar"></span>
                <span class="icon-bar"></span>
            </button>
            <a class="navbar-brand " href=".."><span id="notcoinIcon"><span style="color: #ffff00;">Đ</span></span> <strong><span style="color: #ffff00;">GoldenDoge</span></strong></a>
			<div id="stats_updated"><i class="fa fa-bolt"></i></div>
        </div>

        <div class="collapse navbar-collapse">

            <ul class="nav navbar-nav navbar-left explorer_menu">

			    <li><a class="hot_link" data-page="" href="..">
                    <i class="fa fa-cubes" aria-hidden="true"></i> Explorer
                </a></li>

				<!--
				<li><a class="hot_link" data-page="check_payment.html" href="#check_payment">
                    <i class="fa fa-check-square-o" aria-hidden="true"></i> Check payment
                </a></li>
			
                <li>
				<button rel="css/themes/dark/style.css" class="btn btn-default theme-switch" data-toggle="tooltip" data-placement="bottom" title="" data-original-title="Switch to Night Mode"><i class="fa fa-moon-o"></i></button>
                </li>

                <li>
				<button rel="css/themes/white/style.css" class="btn btn-default theme-switch" data-toggle="tooltip" data-placement="bottom" title="" data-original-title="Switch to Day Mode"><i class="fa fa-sun-o"></i></button>
                </li>-->

	<!-- //-->

                <li style="display:none;"><a class="hot_link" data-page="blockchain_block.html" href="#blockchain_block"><i class="fa fa-cubes"></i> Block
                </a></li>

                <li style="display:none;"><a class="hot_link" data-page="blockchain_transaction.html" href="#blockchain_transaction"><i class="fa fa-cubes"></i> Transaction
                </a></li>

				<li style="display:none;"><a class="hot_link" data-page="blockchain_payment_id.html" href="#blockchain_payment_id"><i class="fa fa-cubes"></i> Transactions by Payment ID
                </a></li>

<!-- //-->
            </ul>


			<div class="nav col-md-6 navbar-right explorer-search">
				<div class="input-group">
					<input class="form-control" placeholder="Search by block height / hash, transaction hash" id="txt_search">
					<span class="input-group-btn"><button class="btn btn-default" type="button" id="btn_search">
						<span><i class="fa fa-search"></i> Search</span>
					</button></span>
				</div>
			</div>



		</div>
	  </div>
</div>


<script>
$('#btn_search').click(function(e) {

var text = document.getElementById('txt_search').value;

function GetSearchBlockbyHeight(){

	var block, xhrGetSearchBlockbyHeight;
    if (xhrGetSearchBlockbyHeight) xhrGetSearchBlockbyHeight.abort();

			xhrGetSearchBlockbyHeight = $.ajax({
            url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "blockbyheight",
                method:"getblockheaderbyheight",
                params: {
                   height: parseInt(text) + 1
                }
            }),
            dataType: 'json',
            cache: 'false',
            success: function(data){
				if(data.result.block_header){
					block = data.result.block_header;
					window.location.href = getBlockchainUrl(block.hash);
				} else if(data.error) {
					wrongSearchAlert();
				}
            }
        });
}

function GetSearchBlock(){
var block, xhrGetSearchBlock;
	if (xhrGetSearchBlock) xhrGetSearchBlock.abort();
		xhrGetSearchBlock = $.ajax({
            url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "",
                method:"get_raw_block",
                params: {
                   hash: text
                }
            }),
            dataType: 'json',
            cache: 'false',
			success: function(data){
				if(data.result){
					block = data.result;
					sessionStorage.setItem('searchBlock', JSON.stringify(block));
					window.location.href = getBlockchainUrl(block.hash);
				} else if(data.error) {
					$.ajax({
						url: api + '/json_rpc',
						method: "POST",
						data: JSON.stringify({
							jsonrpc:"2.0",
							id: "test",
							method:"get_raw_transaction",
							params: {
								hash: text
							}
						}),
						dataType: 'json',
						cache: 'false',
						success: function(data){
							  if(data.result){
								sessionStorage.setItem('searchTransaction', JSON.stringify(data.result));
								window.location.href = transactionExplorer.replace('{id}', text);
							  } else if(data.error) {
								xhrGetTsx =  $.ajax({
									url: api + '/json_rpc',
									method: "POST",
									data: JSON.stringify({
										jsonrpc:"2.0",
										id: "test",
										method:"k_transactions_by_payment_id",
										params: {
											payment_id: text
										}
									}),
									dataType: 'json',
									cache: 'false',
									success: function(data){
										if(data.result){
											txsByPaymentId = data.result.transactions;
											sessionStorage.setItem('txsByPaymentId', JSON.stringify(txsByPaymentId));
											window.location.href = paymentIdExplorer.replace('{id}', text);
										} else if(data.error) {
											$('#page').after(
												'<div class="alert alert-warning alert-dismissable fade in" style="position: fixed; right: 50px; top: 50px;">'+
													'<button type="button" class="close" ' +
															'data-dismiss="alert" aria-hidden="true">' +
														'&times;' +
													'</button>' +
													'We could not find anything.' +
												 '</div>');
										}
									}
								});

							  }
						}
					});
				}
			}
		});
}

if ( text.length < 64 ) {
	GetSearchBlockbyHeight();
} else if ( text.length == 64 ) {
	GetSearchBlock();
} else {
	wrongSearchAlert();
}

e.preventDefault();

});

function wrongSearchAlert() {
	$('#page').after(
		'<div class="alert alert-danger alert-dismissable fade in" style="position: fixed; right: 50px; top: 50px;">'+
		'<button type="button" class="close" ' +
		'data-dismiss="alert" aria-hidden="true">' +
		'&times;' +
		'</button>' +
		'<strong>Wrong search query!</strong><br /> Please enter block height or hash, transaction hash, or payment id.' +
		'</div>');
}

$('#txt_search').keyup(function(e){
        if(e.keyCode === 13)
            $('#btn_search').click();
});
</script>

<div id="content">
	<div class="container">

		<div id="page"></div>

		<p id="loading" class="text-center"><i class="fa fa-circle-o-notch fa-spin"></i></p>

	</div>
</div>

<footer>
	<div class="container">
		<div class="row">
			<div class="col-lg-4 col-md-4 col-sm-6">
				<p>
					<small>
				        <strong>GoldenDoge</strong>
					</small>
				</p>

				<ul>
                    <li><a href="https://GoldenDoge.org/" target="_blank">GoldenDoge.org</a></li>
				</ul>

			</div>
			<div class="col-lg-4 col-md-4 col-sm-6">
				<p>
					<small>


					Based on <a target="_blank" href="https://github.com/zelerius/zelerius-explorer"><i class="fa fa-github"></i> Zelerius Blockchain Explorer</a>
					<br />
					<span class="text-muted">Partially based on <strong>cryptonote-universal-pool</strong><br />
					open sourced under the <a href="http://www.gnu.org/licenses/gpl-2.0.html" target="_blank">GPL</a></span>
					</small>
				</p>
			</div>
			<div class="col-lg-4 col-md-4 col-sm-6">

				<strong class="text-info">Node info</strong>

				<ul class="text-info">
					<li>Status: <span id="node_connection" class="text-danger">Offline</span></li>
					<!--<li>Version: <span id="node_ver">...</span></li>-->
					<li>Height: <span id="node_height">...</span></li>
					<li>Last block: <span id="node_block">...</span></li>
					<li>Difficulty: <span id="node_diff">...</span></li>
					<!--<li>Alt. blocks: <span id="node_alt">...</span></li>-->
					<!--<li>RPC connections: <span id="node_rpc">...</span></li>-->
					<li>Incoming P2P connections: <span id="node_inc">...</span></li>
					<li>Outgoing P2P connects: <span id="node_out">...</span></li>
					<!--<li>White peers: <span id="node_white">...</span></li>-->
					<!--<li>Grey peers: <span id="node_grey">...</span></li>-->
				</ul>

			</div>
		</div>
    </div>
</footer>
<a href="#" class="scrollup"><i class="fa fa-chevron-circle-up"></i></a>
	<script type="text/javascript">
			jQuery(function($) { $(document).ready(function() {
				$(window).scroll(function(){
					if ($(this).scrollTop() > 500) {
						$('.scrollup').fadeIn();
					} else {
						$('.scrollup').fadeOut();
					}
				});

				$('.scrollup').click(function(){
					$("html, body").animate({ scrollTop: 0 }, 600);
					return false;
				});

				$('.scrollup').css('opacity','0.3');

				$('.scrollup').hover(function(){
					$(this).stop().animate({opacity: 0.9}, 400);
				 }, function(){
					$(this).stop().animate({opacity: 0.3}, 400);
				});

			});});
	</script>
</body>
</html>
        <td>GoldenDoge Node &bull; version)";

static const std::string payid_hash_html =
    R"(
<!DOCTYPE html>

<!-- ?v0.04 -->

<html>
<head lang="en">
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, minimum-scale=1">
    <title>GoldenDoge Block Explorer</title>
    <script src="//cdnjs.cloudflare.com/ajax/libs/jquery/2.1.1/jquery.min.js?v0.04"></script>
    <script src="//cdnjs.cloudflare.com/ajax/libs/jquery-timeago/1.4.0/jquery.timeago.min.js?v0.04"></script>
    <script src="//cdnjs.cloudflare.com/ajax/libs/jquery-sparklines/2.1.2/jquery.sparkline.min.js?v0.04"></script>
    <script src="//netdna.bootstrapcdn.com/bootstrap/3.1.1/js/bootstrap.min.js?v0.04"></script>
	<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/2.8.0/Chart.bundle.min.js"></script>
    <link href="//maxcdn.bootstrapcdn.com/font-awesome/4.7.0/css/font-awesome.min.css?v0.04" rel="stylesheet">
    <link href="//fonts.googleapis.com/css?family=Inconsolata" rel="stylesheet" type="text.css?v0.04">
    <script src="../config.js"></script>
	<link href="../css/themes/dark/style.css" rel="stylesheet">
</head>
<body>
<script>

	var blockchainExplorer 	= "../block#{id}";
	var transactionExplorer = "../tx#{id}";
	var paymentIdExplorer 	= "../payid#{id}";

	var style_cookie_name = "stylee";
	var style_cookie_duration = 365;
	var style_domain = window.location.hostname;

	$(function(){
		$('.theme-switch[rel="../css/themes/dark/style.css"]').hide();
		set_style_from_cookie();

		$('.theme-switch').click(function() {
			swapStyleSheet($(this).attr('rel'));
			$('.theme-switch').show();
			$(this).hide();
			return false;
		});

		function swapStyleSheet(sheet){
			$('#theme_link').attr('href',sheet);
			$('.theme-switch').show();
			$('.theme-switch[rel="'+sheet+'"]').hide();
			set_cookie(style_cookie_name, sheet, style_cookie_duration, style_domain);
		}

		function set_style_from_cookie(){
			var style = get_cookie(style_cookie_name);
			if (style.length){
				swapStyleSheet(style);
			}
		}
		function set_cookie (cookie_name, cookie_value, lifespan_in_days, valid_domain){
			var domain_string = valid_domain ?
							   ("; domain=" + valid_domain) : '';
				document.cookie = cookie_name +
							   "=" + encodeURIComponent(cookie_value) +
							   "; max-age=" + 60 * 60 *
							   24 * lifespan_in_days +
							   "; path=/" + domain_string;
		}
		function get_cookie (cookie_name){
			var cookie_string = document.cookie;
			if (cookie_string.length != 0){
				var cookie_value = cookie_string.match(
							  '(^|;)[\s]*' +
							  cookie_name +
							  '=([^;]*)' );
				if(cookie_value != null && cookie_value.length>0) {
					return decodeURIComponent (cookie_value[2]);
				}
			}
			return '';
		}
	});

    function getTransactionUrl(id) {
        return transactionExplorer.replace('{symbol}', symbol.toLowerCase()).replace('{id}', id);
    }

    $.fn.update = function(txt){
        var el = this[0];
        if (el.textContent !== txt)
            el.textContent = txt;
        return this;
    };

    function updateTextClasses(className, text){
        var els = document.getElementsByClassName(className);
        for (var i = 0; i < els.length; i++){
            var el = els[i];
            if (el.textContent !== text)
                el.textContent = text;
        }
    }

    function updateText(elementId, text){
        var el = document.getElementById(elementId);
        if (el.textContent !== text){
            el.textContent = text;
        }
        return el;
    }

    function updateTextLinkable(elementId, text){
        var el = document.getElementById(elementId);
        if (el.innerHTML !== text){
            el.innerHTML = text;
        }
        return el;
    }

    var currentPage;
    var lastStats;
    var sync_blocks;
    var nodeStatus;

    function getReadableHashRateString(hashrate){
        var i = 0;
        var byteUnits = [' H', ' kH', ' MH', ' GH', ' TH', ' PH', ' EH', ' ZH', ' YH' ];
        while (hashrate > 1000){
            hashrate = hashrate / 1000;
            i++;
        }
        return hashrate.toFixed(2) + byteUnits[i];
    }

	function getReadableDifficultyString(difficulty, precision){
		if (isNaN(parseFloat(difficulty)) || !isFinite(difficulty)) return 0;
		if (typeof precision === 'undefined') precision = 2;
		var units = ['', 'k', 'M', 'G', 'T', 'P'],
            number = Math.floor(Math.log(difficulty) / Math.log(1000));
		if (units[number] === undefined || units[number] === null) {
            return 0
        }
        return (difficulty / Math.pow(1000, Math.floor(number))).toFixed(precision) + ' ' +  units[number];
    }

    function formatBlockLink(hash){
        return '<a href="' + getBlockchainUrl(hash) + '" target="_blank">' + hash + '</a>';
    }

    function getReadableCoins(coins, digits, withoutSymbol){
        var amount = (parseInt(coins || 0) / coinUnits).toFixed(digits || coinUnits.toString().length - 1);
        return amount + (withoutSymbol ? '' : (' ' + symbol));
    }

    function formatDate(time){
        if (!time) return '';
        return new Date(parseInt(time) * 1000).toLocaleString();
    }

	function formatBytes(a,b) {
		if(0==a)return"0 Bytes";var c=1024,d=b||2,e=["Bytes","KB","MB","GB","TB","PB","EB","ZB","YB"],f=Math.floor(Math.log(a)/Math.log(c));return parseFloat((a/Math.pow(c,f)).toFixed(d))+" "+e[f]
	}

	function formatPaymentLink(hash){
        return '<a href="' + getTransactionUrl(hash) + '" target="_blank">' + hash + '</a>';
    }

    function pulseLiveUpdate(){
        var stats_update = document.getElementById('stats_updated');
        stats_update.style.transition = 'opacity 100ms ease-out';
        stats_update.style.opacity = 1;
        setTimeout(function(){
            stats_update.style.transition = 'opacity 7000ms linear';
            stats_update.style.opacity = 0;
        }, 500);
    }

    window.onhashchange = function(){
        routePage();
    };


    function fetchLiveStats() {
        $.ajax({
            url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "",
                method:"get_status",
                params: {
                }
            }),
            dataType: 'json',
            cache: 'false'
        }).done(function(data, success){
            pulseLiveUpdate();
            lastStats = data;
			nodeStatus = success;
            currentPage.update();
			nodeInfo();
        }).always(function () {
			setTimeout(function() {
				fetchLiveStats();
			}, refreshDelay);
        });
    }

    function floatToString(float) {
        return float.toFixed(6).replace(/[0\.]+$/, '');
    }

	function nodeInfo() {
        if(nodeStatus) {

			$('#node_connection').html('Online').addClass('text-success').removeClass('text-danger');
			$('#node_height').html(parseInt(lastStats.result.top_block_height));
			$('#node_block').html(parseInt(lastStats.result.top_known_block_height));
			$('#node_diff').html(parseInt(lastStats.result.top_block_difficulty));
			//$('#node_alt').html(parseInt(lastStats['alt_blocks_count']));
			//$('#node_rpc').html(parseInt(lastStats['rpc_connections_count']));
			$('#node_inc').html(parseInt(lastStats.result.incoming_peer_count));
			$('#node_out').html(parseInt(lastStats.result.outgoing_peer_count));
			//$('#node_white').html(parseInt(lastStats['white_peerlist_size']));
			//$('#node_grey').html(parseInt(lastStats['grey_peerlist_size']));
			//if (lastStats['version'] !== 'undefined'){
				//$('#node_ver').html(lastStats.result.transaction_pool_version);
			//}
		} else {
			$('#node_connection').html('Offline').addClass('text-danger').removeClass('text-success');
		}
    }

    var xhrPageLoading;
    function routePage(loadedCallback) {

        if (currentPage) currentPage.destroy();
        $('#page').html('');
        $('#loading').show();

        if (xhrPageLoading)
            xhrPageLoading.abort();

        $('.hot_link').parent().removeClass('active');
        var $link = $('a.hot_link[href="' + (window.location.hash || '#') + '"]');

        $link.parent().addClass('active');
        var page = $link.data('page');

        xhrPageLoading = $.ajax({
            url: '../pages/' + 'blockchain_payment_id.html',
            cache: true,
            success: function (data) {
                $('#loading').hide();
                $('#page').show().html(data);
                currentPage.init();
                currentPage.update();
                if (loadedCallback) loadedCallback();
            }
        });
    }

    function getBlockchainUrl(id) {
        return blockchainExplorer.replace('{id}', id);
	}

	function getinfo() {
		$.ajax({
			url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "",
                method:"get_status",
                params: {
                }
            }),
            dataType: 'json',
            cache: 'false',
			timeout: 1500 //in milliseconds
		})
		.done(function (data) {
			try {
				lastStats = JSON.parse(data);
			} catch (e) {
				lastStats = data;
			}
			routePage(fetchLiveStats);
		})
		.fail(function () {
			//apiList.push(api);
			//api = apiList.shift();
			getinfo();
		});
	}



    $(function(){
		getinfo();
    });

    // Blockexplorer functions
    urlParam = function(name){
        var results = new RegExp('[\?&]' + name + '=([^&#]*)').exec(window.location.href);
        if (results==null){
           return null;
        }
        else{
           return results[1] || 0;
        }
    }

	$(function() {
        $('[data-toggle="tooltip"]').tooltip();
    });

	function hex2a(hexx) {
		var hex = hexx.toString();//force conversion
		var str = '';
		for (var i = 0; i < hex.length; i += 2)
			str += String.fromCharCode(parseInt(hex.substr(i, 2), 16));
		return str;
	}
</script>

<div class="navbar navbar-default navbar-fixed-top" role="navigation">
    <div class="container">
        <div class="navbar-header">
            <button type="button" class="navbar-toggle" data-toggle="collapse" data-target=".navbar-collapse">
                <span class="sr-only">Menu</span>
                <span class="icon-bar"></span>
                <span class="icon-bar"></span>
                <span class="icon-bar"></span>
            </button>
            <a class="navbar-brand " href=".."><span id="notcoinIcon"><span style="color: #ffff00;">Đ</span></span> <strong><span style="color: #ffff00;">GoldenDoge</span></strong></a>
			<div id="stats_updated"><i class="fa fa-bolt"></i></div>
        </div>

        <div class="collapse navbar-collapse">

            <ul class="nav navbar-nav navbar-left explorer_menu">

			    <li><a class="hot_link" data-page="" href="..">
                    <i class="fa fa-cubes" aria-hidden="true"></i> Explorer
                </a></li>

				<!--
				<li><a class="hot_link" data-page="check_payment.html" href="#check_payment">
                    <i class="fa fa-check-square-o" aria-hidden="true"></i> Check payment
                </a></li>
			
                <li>
				<button rel="css/themes/dark/style.css" class="btn btn-default theme-switch" data-toggle="tooltip" data-placement="bottom" title="" data-original-title="Switch to Night Mode"><i class="fa fa-moon-o"></i></button>
                </li>

                <li>
				<button rel="css/themes/white/style.css" class="btn btn-default theme-switch" data-toggle="tooltip" data-placement="bottom" title="" data-original-title="Switch to Day Mode"><i class="fa fa-sun-o"></i></button>
                </li>-->

	<!-- //-->

                <li style="display:none;"><a class="hot_link" data-page="blockchain_block.html" href="#blockchain_block"><i class="fa fa-cubes"></i> Block
                </a></li>

                <li style="display:none;"><a class="hot_link" data-page="blockchain_transaction.html" href="#blockchain_transaction"><i class="fa fa-cubes"></i> Transaction
                </a></li>

				<li style="display:none;"><a class="hot_link" data-page="blockchain_payment_id.html" href="#blockchain_payment_id"><i class="fa fa-cubes"></i> Transactions by Payment ID
                </a></li>

<!-- //-->
            </ul>


			<div class="nav col-md-6 navbar-right explorer-search">
				<div class="input-group">
					<input class="form-control" placeholder="Search by block height / hash, transaction hash" id="txt_search">
					<span class="input-group-btn"><button class="btn btn-default" type="button" id="btn_search">
						<span><i class="fa fa-search"></i> Search</span>
					</button></span>
				</div>
			</div>



		</div>
	  </div>
</div>


<script>
$('#btn_search').click(function(e) {

var text = document.getElementById('txt_search').value;

function GetSearchBlockbyHeight(){

	var block, xhrGetSearchBlockbyHeight;
    if (xhrGetSearchBlockbyHeight) xhrGetSearchBlockbyHeight.abort();

			xhrGetSearchBlockbyHeight = $.ajax({
            url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "blockbyheight",
                method:"getblockheaderbyheight",
                params: {
                   height: parseInt(text) + 1
                }
            }),
            dataType: 'json',
            cache: 'false',
            success: function(data){
				if(data.result.block_header){
					block = data.result.block_header;
					window.location.href = getBlockchainUrl(block.hash);
				} else if(data.error) {
					wrongSearchAlert();
				}
            }
        });
}

function GetSearchBlock(){
var block, xhrGetSearchBlock;
	if (xhrGetSearchBlock) xhrGetSearchBlock.abort();
		xhrGetSearchBlock = $.ajax({
            url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "",
                method:"get_raw_block",
                params: {
                   hash: text
                }
            }),
            dataType: 'json',
            cache: 'false',
			success: function(data){
				if(data.result){
					block = data.result;
					sessionStorage.setItem('searchBlock', JSON.stringify(block));
					window.location.href = getBlockchainUrl(block.hash);
				} else if(data.error) {
					$.ajax({
						url: api + '/json_rpc',
						method: "POST",
						data: JSON.stringify({
							jsonrpc:"2.0",
							id: "test",
							method:"get_raw_transaction",
							params: {
								hash: text
							}
						}),
						dataType: 'json',
						cache: 'false',
						success: function(data){
							  if(data.result){
								sessionStorage.setItem('searchTransaction', JSON.stringify(data.result));
								window.location.href = transactionExplorer.replace('{id}', text);
							  } else if(data.error) {
								xhrGetTsx =  $.ajax({
									url: api + '/json_rpc',
									method: "POST",
									data: JSON.stringify({
										jsonrpc:"2.0",
										id: "test",
										method:"k_transactions_by_payment_id",
										params: {
											payment_id: text
										}
									}),
									dataType: 'json',
									cache: 'false',
									success: function(data){
										if(data.result){
											txsByPaymentId = data.result.transactions;
											sessionStorage.setItem('txsByPaymentId', JSON.stringify(txsByPaymentId));
											window.location.href = paymentIdExplorer.replace('{id}', text);
										} else if(data.error) {
											$('#page').after(
												'<div class="alert alert-warning alert-dismissable fade in" style="position: fixed; right: 50px; top: 50px;">'+
													'<button type="button" class="close" ' +
															'data-dismiss="alert" aria-hidden="true">' +
														'&times;' +
													'</button>' +
													'We could not find anything.' +
												 '</div>');
										}
									}
								});

							  }
						}
					});
				}
			}
		});
}

if ( text.length < 64 ) {
	GetSearchBlockbyHeight();
} else if ( text.length == 64 ) {
	GetSearchBlock();
} else {
	wrongSearchAlert();
}

e.preventDefault();

});

function wrongSearchAlert() {
	$('#page').after(
		'<div class="alert alert-danger alert-dismissable fade in" style="position: fixed; right: 50px; top: 50px;">'+
		'<button type="button" class="close" ' +
		'data-dismiss="alert" aria-hidden="true">' +
		'&times;' +
		'</button>' +
		'<strong>Wrong search query!</strong><br /> Please enter block height or hash, transaction hash, or payment id.' +
		'</div>');
}

$('#txt_search').keyup(function(e){
        if(e.keyCode === 13)
            $('#btn_search').click();
});
</script>

<div id="content">
	<div class="container">

		<div id="page"></div>

		<p id="loading" class="text-center"><i class="fa fa-circle-o-notch fa-spin"></i></p>

	</div>
</div>

<footer>
	<div class="container">
		<div class="row">
			<div class="col-lg-4 col-md-4 col-sm-6">
				<p>
					<small>
				        <strong>GoldenDoge</strong>
					</small>
				</p>

				<ul>
                    <li><a href="https://GoldenDoge.org/" target="_blank">GoldenDoge.org</a></li>
				</ul>

			</div>
			<div class="col-lg-4 col-md-4 col-sm-6">
				<p>
					<small>


					Based on <a target="_blank" href="https://github.com/zelerius/zelerius-explorer"><i class="fa fa-github"></i> Zelerius Blockchain Explorer</a>
					<br />
					<span class="text-muted">Partially based on <strong>cryptonote-universal-pool</strong><br />
					open sourced under the <a href="http://www.gnu.org/licenses/gpl-2.0.html" target="_blank">GPL</a></span>
					</small>
				</p>
			</div>
			<div class="col-lg-4 col-md-4 col-sm-6">

				<strong class="text-info">Node info</strong>

				<ul class="text-info">
					<li>Status: <span id="node_connection" class="text-danger">Offline</span></li>
					<!--<li>Version: <span id="node_ver">...</span></li>-->
					<li>Height: <span id="node_height">...</span></li>
					<li>Last block: <span id="node_block">...</span></li>
					<li>Difficulty: <span id="node_diff">...</span></li>
					<!--<li>Alt. blocks: <span id="node_alt">...</span></li>-->
					<!--<li>RPC connections: <span id="node_rpc">...</span></li>-->
					<li>Incoming P2P connections: <span id="node_inc">...</span></li>
					<li>Outgoing P2P connects: <span id="node_out">...</span></li>
					<!--<li>White peers: <span id="node_white">...</span></li>-->
					<!--<li>Grey peers: <span id="node_grey">...</span></li>-->
				</ul>

			</div>
		</div>
    </div>
</footer>
<a href="#" class="scrollup"><i class="fa fa-chevron-circle-up"></i></a>
	<script type="text/javascript">
			jQuery(function($) { $(document).ready(function() {
				$(window).scroll(function(){
					if ($(this).scrollTop() > 500) {
						$('.scrollup').fadeIn();
					} else {
						$('.scrollup').fadeOut();
					}
				});

				$('.scrollup').click(function(){
					$("html, body").animate({ scrollTop: 0 }, 600);
					return false;
				});

				$('.scrollup').css('opacity','0.3');

				$('.scrollup').hover(function(){
					$(this).stop().animate({opacity: 0.9}, 400);
				 }, function(){
					$(this).stop().animate({opacity: 0.3}, 400);
				});

			});});
	</script>
</body>
</html>
        <td>GoldenDoge Node &bull; version)";

static const std::string config_js =
    R"(var api = 'http://nbr.m2pool.eu:4041';
var apiList = ["http://nbr.m2pool.eu:4041"];
var blockTargetInterval = 20;
var coinUnits = 10000000000000;
var symbol = 'GDOGE';
var refreshDelay = 10000;
var whiteTheme = "css/themes/dark-theme.css";
var nightTheme = "css/themes/dark-theme.css";
// pools stats by MainCoins
var poolsStat = 
	[
		[]
    ];
var nodesStat = 
	[
        []
    ];
)";

static const std::string cookie_js =
    R"(/*!
 * jQuery Cookie Plugin v1.4.1
 * https://github.com/carhartl/jquery-cookie
 *
 * Copyright 2006, 2014 Klaus Hartl
 * Released under the MIT license
 */
(function (factory) {
	if (typeof define === 'function' && define.amd) {
		// AMD (Register as an anonymous module)
		define(['jquery'], factory);
	} else if (typeof exports === 'object') {
		// Node/CommonJS
		module.exports = factory(require('jquery'));
	} else {
		// Browser globals
		factory(jQuery);
	}
}(function ($) {

	var pluses = /\+/g;

	function encode(s) {
		return config.raw ? s : encodeURIComponent(s);
	}

	function decode(s) {
		return config.raw ? s : decodeURIComponent(s);
	}

	function stringifyCookieValue(value) {
		return encode(config.json ? JSON.stringify(value) : String(value));
	}

	function parseCookieValue(s) {
		if (s.indexOf('"') === 0) {
			// This is a quoted cookie as according to RFC2068, unescape...
			s = s.slice(1, -1).replace(/\\"/g, '"').replace(/\\\\/g, '\\');
		}

		try {
			// Replace server-side written pluses with spaces.
			// If we can't decode the cookie, ignore it, it's unusable.
			// If we can't parse the cookie, ignore it, it's unusable.
			s = decodeURIComponent(s.replace(pluses, ' '));
			return config.json ? JSON.parse(s) : s;
		} catch(e) {}
	}

	function read(s, converter) {
		var value = config.raw ? s : parseCookieValue(s);
		return $.isFunction(converter) ? converter(value) : value;
	}

	var config = $.cookie = function (key, value, options) {

		// Write

		if (arguments.length > 1 && !$.isFunction(value)) {
			options = $.extend({}, config.defaults, options);

			if (typeof options.expires === 'number') {
				var days = options.expires, t = options.expires = new Date();
				t.setMilliseconds(t.getMilliseconds() + days * 864e+5);
			}

			return (document.cookie = [
				encode(key), '=', stringifyCookieValue(value),
				options.expires ? '; expires=' + options.expires.toUTCString() : '', // use expires attribute, max-age is not supported by IE
				options.path    ? '; path=' + options.path : '',
				options.domain  ? '; domain=' + options.domain : '',
				options.secure  ? '; secure' : ''
			].join(''));
		}

		// Read

		var result = key ? undefined : {},
			// To prevent the for loop in the first place assign an empty array
			// in case there are no cookies at all. Also prevents odd result when
			// calling $.cookie().
			cookies = document.cookie ? document.cookie.split('; ') : [],
			i = 0,
			l = cookies.length;

		for (; i < l; i++) {
			var parts = cookies[i].split('='),
				name = decode(parts.shift()),
				cookie = parts.join('=');

			if (key === name) {
				// If second argument (value) is a function it's a converter...
				result = read(cookie, value);
				break;
			}

			// Prevent storing a cookie that we couldn't decode.
			if (!key && (cookie = read(cookie)) !== undefined) {
				result[name] = cookie;
			}
		}

		return result;
	};

	config.defaults = {};

	$.removeCookie = function (key, options) {
		// Must not alter options, thus extending a fresh object...
		$.cookie(key, '', $.extend({}, options, { expires: -1 }));
		return !$.cookie(key);
	};

}));
)";

static const std::string sorttable_js =
    R"(
		/*
  SortTable
  version 2
  7th April 2007
  Stuart Langridge, http://www.kryogenix.org/code/browser/sorttable/

  Instructions:
  Download this file
  Add <script src="sorttable.js"></script> to your HTML
  Add class="sortable" to any table you'd like to make sortable
  Click on the headers to sort

  Thanks to many, many people for contributions and suggestions.
  Licenced as X11: http://www.kryogenix.org/code/browser/licence.html
  This basically means: do what you want with it.
*/


var stIsIE = /*@cc_on!@*/false;

sorttable = {
  init: function() {
    // quit if this function has already been called
    if (arguments.callee.done) return;
    // flag this function so we don't do the same thing twice
    arguments.callee.done = true;
    // kill the timer
    if (_timer) clearInterval(_timer);

    if (!document.createElement || !document.getElementsByTagName) return;

    sorttable.DATE_RE = /^(\d\d?)[\/\.-](\d\d?)[\/\.-]((\d\d)?\d\d)$/;

    forEach(document.getElementsByTagName('table'), function(table) {
      if (table.className.search(/\bsortable\b/) != -1) {
        sorttable.makeSortable(table);
      }
    });

  },

  makeSortable: function(table) {
    if (table.getElementsByTagName('thead').length == 0) {
      // table doesn't have a tHead. Since it should have, create one and
      // put the first table row in it.
      the = document.createElement('thead');
      the.appendChild(table.rows[0]);
      table.insertBefore(the,table.firstChild);
    }
    // Safari doesn't support table.tHead, sigh
    if (table.tHead == null) table.tHead = table.getElementsByTagName('thead')[0];

    if (table.tHead.rows.length != 1) return; // can't cope with two header rows

    // Sorttable v1 put rows with a class of "sortbottom" at the bottom (as
    // "total" rows, for example). This is B&R, since what you're supposed
    // to do is put them in a tfoot. So, if there are sortbottom rows,
    // for backwards compatibility, move them to tfoot (creating it if needed).
    sortbottomrows = [];
    for (var i=0; i<table.rows.length; i++) {
      if (table.rows[i].className.search(/\bsortbottom\b/) != -1) {
        sortbottomrows[sortbottomrows.length] = table.rows[i];
      }
    }
    if (sortbottomrows) {
      if (table.tFoot == null) {
        // table doesn't have a tfoot. Create one.
        tfo = document.createElement('tfoot');
        table.appendChild(tfo);
      }
      for (var i=0; i<sortbottomrows.length; i++) {
        tfo.appendChild(sortbottomrows[i]);
      }
      delete sortbottomrows;
    }

    // work through each column and calculate its type
    headrow = table.tHead.rows[0].cells;
    for (var i=0; i<headrow.length; i++) {
      // manually override the type with a sorttable_type attribute
      if (!headrow[i].className.match(/\bsorttable_nosort\b/)) { // skip this col
        mtch = headrow[i].className.match(/\bsorttable_([a-z0-9]+)\b/);
        if (mtch) { override = mtch[1]; }
	      if (mtch && typeof sorttable["sort_"+override] == 'function') {
	        headrow[i].sorttable_sortfunction = sorttable["sort_"+override];
	      } else {
	        headrow[i].sorttable_sortfunction = sorttable.guessType(table,i);
	      }
	      // make it clickable to sort
	      headrow[i].sorttable_columnindex = i;
	      headrow[i].sorttable_tbody = table.tBodies[0];
	      dean_addEvent(headrow[i],"click", sorttable.innerSortFunction = function(e) {

          if (this.className.search(/\bsorttable_sorted\b/) != -1) {
            // if we're already sorted by this column, just
            // reverse the table, which is quicker
            sorttable.reverse(this.sorttable_tbody);
            this.className = this.className.replace('sorttable_sorted',
                                                    'sorttable_sorted_reverse');
            this.removeChild(document.getElementById('sorttable_sortfwdind'));
            sortrevind = document.createElement('span');
            sortrevind.id = "sorttable_sortrevind";
            sortrevind.innerHTML = stIsIE ? '&nbsp<font face="webdings">5</font>' : '&nbsp;&#x25B4;';
            this.appendChild(sortrevind);
            return;
          }
          if (this.className.search(/\bsorttable_sorted_reverse\b/) != -1) {
            // if we're already sorted by this column in reverse, just
            // re-reverse the table, which is quicker
            sorttable.reverse(this.sorttable_tbody);
            this.className = this.className.replace('sorttable_sorted_reverse',
                                                    'sorttable_sorted');
            this.removeChild(document.getElementById('sorttable_sortrevind'));
            sortfwdind = document.createElement('span');
            sortfwdind.id = "sorttable_sortfwdind";
            sortfwdind.innerHTML = stIsIE ? '&nbsp<font face="webdings">6</font>' : '&nbsp;&#x25BE;';
            this.appendChild(sortfwdind);
            return;
          }

          // remove sorttable_sorted classes
          theadrow = this.parentNode;
          forEach(theadrow.childNodes, function(cell) {
            if (cell.nodeType == 1) { // an element
              cell.className = cell.className.replace('sorttable_sorted_reverse','');
              cell.className = cell.className.replace('sorttable_sorted','');
            }
          });
          sortfwdind = document.getElementById('sorttable_sortfwdind');
          if (sortfwdind) { sortfwdind.parentNode.removeChild(sortfwdind); }
          sortrevind = document.getElementById('sorttable_sortrevind');
          if (sortrevind) { sortrevind.parentNode.removeChild(sortrevind); }

          this.className += ' sorttable_sorted';
          sortfwdind = document.createElement('span');
          sortfwdind.id = "sorttable_sortfwdind";
          sortfwdind.innerHTML = stIsIE ? '&nbsp<font face="webdings">6</font>' : '&nbsp;&#x25BE;';
          this.appendChild(sortfwdind);

	        // build an array to sort. This is a Schwartzian transform thing,
	        // i.e., we "decorate" each row with the actual sort key,
	        // sort based on the sort keys, and then put the rows back in order
	        // which is a lot faster because you only do getInnerText once per row
	        row_array = [];
	        col = this.sorttable_columnindex;
	        rows = this.sorttable_tbody.rows;
	        for (var j=0; j<rows.length; j++) {
	          row_array[row_array.length] = [sorttable.getInnerText(rows[j].cells[col]), rows[j]];
	        }
	        /* If you want a stable sort, uncomment the following line */
	        //sorttable.shaker_sort(row_array, this.sorttable_sortfunction);
	        /* and comment out this one */
	        row_array.sort(this.sorttable_sortfunction);

	        tb = this.sorttable_tbody;
	        for (var j=0; j<row_array.length; j++) {
	          tb.appendChild(row_array[j][1]);
	        }

	        delete row_array;
	      });
	    }
    }
  },

  guessType: function(table, column) {
    // guess the type of a column based on its first non-blank row
    sortfn = sorttable.sort_alpha;
    for (var i=0; i<table.tBodies[0].rows.length; i++) {
      text = sorttable.getInnerText(table.tBodies[0].rows[i].cells[column]);
      if (text != '') {
        if (text.match(/^-?[�$�]?[\d,.]+%?$/)) {
          return sorttable.sort_numeric;
        }
        // check for a date: dd/mm/yyyy or dd/mm/yy
        // can have / or . or - as separator
        // can be mm/dd as well
        possdate = text.match(sorttable.DATE_RE)
        if (possdate) {
          // looks like a date
          first = parseInt(possdate[1]);
          second = parseInt(possdate[2]);
          if (first > 12) {
            // definitely dd/mm
            return sorttable.sort_ddmm;
          } else if (second > 12) {
            return sorttable.sort_mmdd;
          } else {
            // looks like a date, but we can't tell which, so assume
            // that it's dd/mm (English imperialism!) and keep looking
            sortfn = sorttable.sort_ddmm;
          }
        }
      }
    }
    return sortfn;
  },

  getInnerText: function(node) {
    // gets the text we want to use for sorting for a cell.
    // strips leading and trailing whitespace.
    // this is *not* a generic getInnerText function; it's special to sorttable.
    // for example, you can override the cell text with a customkey attribute.
    // it also gets .value for <input> fields.

    if (!node) return "";

    hasInputs = (typeof node.getElementsByTagName == 'function') &&
                 node.getElementsByTagName('input').length;

    if (node.getAttribute("sorttable_customkey") != null) {
      return node.getAttribute("sorttable_customkey");
    }
    else if (typeof node.textContent != 'undefined' && !hasInputs) {
      return node.textContent.replace(/^\s+|\s+$/g, '');
    }
    else if (typeof node.innerText != 'undefined' && !hasInputs) {
      return node.innerText.replace(/^\s+|\s+$/g, '');
    }
    else if (typeof node.text != 'undefined' && !hasInputs) {
      return node.text.replace(/^\s+|\s+$/g, '');
    }
    else {
      switch (node.nodeType) {
        case 3:
          if (node.nodeName.toLowerCase() == 'input') {
            return node.value.replace(/^\s+|\s+$/g, '');
          }
        case 4:
          return node.nodeValue.replace(/^\s+|\s+$/g, '');
          break;
        case 1:
        case 11:
          var innerText = '';
          for (var i = 0; i < node.childNodes.length; i++) {
            innerText += sorttable.getInnerText(node.childNodes[i]);
          }
          return innerText.replace(/^\s+|\s+$/g, '');
          break;
        default:
          return '';
      }
    }
  },

  reverse: function(tbody) {
    // reverse the rows in a tbody
    newrows = [];
    for (var i=0; i<tbody.rows.length; i++) {
      newrows[newrows.length] = tbody.rows[i];
    }
    for (var i=newrows.length-1; i>=0; i--) {
       tbody.appendChild(newrows[i]);
    }
    delete newrows;
  },

  /* sort functions
     each sort function takes two parameters, a and b
     you are comparing a[0] and b[0] */
  sort_numeric: function(a,b) {
    aa = parseFloat(a[0].replace(/[^0-9.-]/g,''));
    if (isNaN(aa)) aa = 0;
    bb = parseFloat(b[0].replace(/[^0-9.-]/g,''));
    if (isNaN(bb)) bb = 0;
    return aa-bb;
  },
  sort_alpha: function(a,b) {
    if (a[0]==b[0]) return 0;
    if (a[0]<b[0]) return -1;
    return 1;
  },
  sort_ddmm: function(a,b) {
    mtch = a[0].match(sorttable.DATE_RE);
    y = mtch[3]; m = mtch[2]; d = mtch[1];
    if (m.length == 1) m = '0'+m;
    if (d.length == 1) d = '0'+d;
    dt1 = y+m+d;
    mtch = b[0].match(sorttable.DATE_RE);
    y = mtch[3]; m = mtch[2]; d = mtch[1];
    if (m.length == 1) m = '0'+m;
    if (d.length == 1) d = '0'+d;
    dt2 = y+m+d;
    if (dt1==dt2) return 0;
    if (dt1<dt2) return -1;
    return 1;
  },
  sort_mmdd: function(a,b) {
    mtch = a[0].match(sorttable.DATE_RE);
    y = mtch[3]; d = mtch[2]; m = mtch[1];
    if (m.length == 1) m = '0'+m;
    if (d.length == 1) d = '0'+d;
    dt1 = y+m+d;
    mtch = b[0].match(sorttable.DATE_RE);
    y = mtch[3]; d = mtch[2]; m = mtch[1];
    if (m.length == 1) m = '0'+m;
    if (d.length == 1) d = '0'+d;
    dt2 = y+m+d;
    if (dt1==dt2) return 0;
    if (dt1<dt2) return -1;
    return 1;
  },

  shaker_sort: function(list, comp_func) {
    // A stable sort function to allow multi-level sorting of data
    // see: http://en.wikipedia.org/wiki/Cocktail_sort
    // thanks to Joseph Nahmias
    var b = 0;
    var t = list.length - 1;
    var swap = true;

    while(swap) {
        swap = false;
        for(var i = b; i < t; ++i) {
            if ( comp_func(list[i], list[i+1]) > 0 ) {
                var q = list[i]; list[i] = list[i+1]; list[i+1] = q;
                swap = true;
            }
        } // for
        t--;

        if (!swap) break;

        for(var i = t; i > b; --i) {
            if ( comp_func(list[i], list[i-1]) < 0 ) {
                var q = list[i]; list[i] = list[i-1]; list[i-1] = q;
                swap = true;
            }
        } // for
        b++;

    } // while(swap)
  }
}

/* ******************************************************************
   Supporting functions: bundled here to avoid depending on a library
   ****************************************************************** */

// Dean Edwards/Matthias Miller/John Resig

/* for Mozilla/Opera9 */
if (document.addEventListener) {
    document.addEventListener("DOMContentLoaded", sorttable.init, false);
}

/* for Internet Explorer */
/*@cc_on @*/
/*@if (@_win32)
    document.write("<script id=__ie_onload defer src=javascript:void(0)><\/script>");
    var script = document.getElementById("__ie_onload");
    script.onreadystatechange = function() {
        if (this.readyState == "complete") {
            sorttable.init(); // call the onload handler
        }
    };
/*@end @*/

/* for Safari */
if (/WebKit/i.test(navigator.userAgent)) { // sniff
    var _timer = setInterval(function() {
        if (/loaded|complete/.test(document.readyState)) {
            sorttable.init(); // call the onload handler
        }
    }, 10);
}

/* for other browsers */
window.onload = sorttable.init;

// written by Dean Edwards, 2005
// with input from Tino Zijdel, Matthias Miller, Diego Perini

// http://dean.edwards.name/weblog/2005/10/add-event/

function dean_addEvent(element, type, handler) {
	if (element.addEventListener) {
		element.addEventListener(type, handler, false);
	} else {
		// assign each event handler a unique ID
		if (!handler.$$guid) handler.$$guid = dean_addEvent.guid++;
		// create a hash table of event types for the element
		if (!element.events) element.events = {};
		// create a hash table of event handlers for each element/event pair
		var handlers = element.events[type];
		if (!handlers) {
			handlers = element.events[type] = {};
			// store the existing event handler (if there is one)
			if (element["on" + type]) {
				handlers[0] = element["on" + type];
			}
		}
		// store the event handler in the hash table
		handlers[handler.$$guid] = handler;
		// assign a global event handler to do all the work
		element["on" + type] = handleEvent;
	}
};
// a counter used to create unique IDs
dean_addEvent.guid = 1;

function removeEvent(element, type, handler) {
	if (element.removeEventListener) {
		element.removeEventListener(type, handler, false);
	} else {
		// delete the event handler from the hash table
		if (element.events && element.events[type]) {
			delete element.events[type][handler.$$guid];
		}
	}
};

function handleEvent(event) {
	var returnValue = true;
	// grab the event object (IE uses a global event object)
	event = event || fixEvent(((this.ownerDocument || this.document || this).parentWindow || window).event);
	// get a reference to the hash table of event handlers
	var handlers = this.events[event.type];
	// execute each event handler
	for (var i in handlers) {
		this.$$handleEvent = handlers[i];
		if (this.$$handleEvent(event) === false) {
			returnValue = false;
		}
	}
	return returnValue;
};

function fixEvent(event) {
	// add W3C standard event methods
	event.preventDefault = fixEvent.preventDefault;
	event.stopPropagation = fixEvent.stopPropagation;
	return event;
};
fixEvent.preventDefault = function() {
	this.returnValue = false;
};
fixEvent.stopPropagation = function() {
  this.cancelBubble = true;
}

// Dean's forEach: http://dean.edwards.name/base/forEach.js
/*
	forEach, version 1.0
	Copyright 2006, Dean Edwards
	License: http://www.opensource.org/licenses/mit-license.php
*/

// array-like enumeration
if (!Array.forEach) { // mozilla already supports this
	Array.forEach = function(array, block, context) {
		for (var i = 0; i < array.length; i++) {
			block.call(context, array[i], i, array);
		}
	};
}

// generic enumeration
Function.prototype.forEach = function(object, block, context) {
	for (var key in object) {
		if (typeof this.prototype[key] == "undefined") {
			block.call(context, object[key], key, object);
		}
	}
};

// character enumeration
String.forEach = function(string, block, context) {
	Array.forEach(string.split(""), function(chr, index) {
		block.call(context, chr, index, string);
	});
};

// globally resolve forEach enumeration
var forEach = function(object, block, context) {
	if (object) {
		var resolve = Object; // default
		if (object instanceof Function) {
			// functions have a "length" property
			resolve = Function;
		} else if (object.forEach instanceof Function) {
			// the object implements a custom forEach method so use that
			object.forEach(block, context);
			return;
		} else if (typeof object == "string") {
			// the object is a string
			resolve = String;
		} else if (typeof object.length == "number") {
			// the object is array-like
			resolve = Array;
		}
		resolve.forEach(object, block, context);
	}
};


		
	)";

	static const std::string bootstrap_min_css =
    R"(/*!
 * bootswatch v3.3.7
 * Homepage: http://bootswatch.com
 * Copyright 2012-2016 Thomas Park
 * Licensed under MIT
 * Based on Bootstrap
*//*!
 * Bootstrap v3.3.7 (http://getbootstrap.com)
 * Copyright 2011-2016 Twitter, Inc.
 * Licensed under MIT (https://github.com/twbs/bootstrap/blob/master/LICENSE)
 *//*! normalize.css v3.0.3 | MIT License | github.com/necolas/normalize.css */html{font-family:sans-serif;-ms-text-size-adjust:100%;-webkit-text-size-adjust:100%}body{margin:0}article,aside,details,figcaption,figure,footer,header,hgroup,main,menu,nav,section,summary{display:block}audio,canvas,progress,video{display:inline-block;vertical-align:baseline}audio:not([controls]){display:none;height:0}[hidden],template{display:none}a{background-color:transparent}a:active,a:hover{outline:0}abbr[title]{border-bottom:1px dotted}b,strong{font-weight:bold}dfn{font-style:italic}h1{font-size:2em;margin:0.67em 0}mark{background:#ff0;color:#000}small{font-size:80%}sub,sup{font-size:75%;line-height:0;position:relative;vertical-align:baseline}sup{top:-0.5em}sub{bottom:-0.25em}img{border:0}svg:not(:root){overflow:hidden}figure{margin:1em 40px}hr{-webkit-box-sizing:content-box;-moz-box-sizing:content-box;box-sizing:content-box;height:0}pre{overflow:auto}code,kbd,pre,samp{font-family:monospace, monospace;font-size:1em}button,input,optgroup,select,textarea{color:inherit;font:inherit;margin:0}button{overflow:visible}button,select{text-transform:none}button,html input[type="button"],input[type="reset"],input[type="submit"]{-webkit-appearance:button;cursor:pointer}button[disabled],html input[disabled]{cursor:default}button::-moz-focus-inner,input::-moz-focus-inner{border:0;padding:0}input{line-height:normal}input[type="checkbox"],input[type="radio"]{-webkit-box-sizing:border-box;-moz-box-sizing:border-box;box-sizing:border-box;padding:0}input[type="number"]::-webkit-inner-spin-button,input[type="number"]::-webkit-outer-spin-button{height:auto}input[type="search"]{-webkit-appearance:textfield;-webkit-box-sizing:content-box;-moz-box-sizing:content-box;box-sizing:content-box}input[type="search"]::-webkit-search-cancel-button,input[type="search"]::-webkit-search-decoration{-webkit-appearance:none}fieldset{border:1px solid #c0c0c0;margin:0 2px;padding:0.35em 0.625em 0.75em}legend{border:0;padding:0}textarea{overflow:auto}optgroup{font-weight:bold}table{border-collapse:collapse;border-spacing:0}td,th{padding:0}/*! Source: https://github.com/h5bp/html5-boilerplate/blob/master/src/css/main.css */@media print{*,*:before,*:after{background:transparent !important;color:#000 !important;-webkit-box-shadow:none !important;box-shadow:none !important;text-shadow:none !important}a,a:visited{text-decoration:underline}a[href]:after{content:" "}a[href^="#"]:after,a[href^="javascript:"]:after{content:""}pre,blockquote{border:1px solid #999;page-break-inside:avoid}thead{display:table-header-group}tr,img{page-break-inside:avoid}img{max-width:100% !important}p,h2,h3{orphans:3;widows:3}h2,h3{page-break-after:avoid}.navbar{display:none}.btn>.caret,.dropup>.btn>.caret{border-top-color:#000 !important}.label{border:1px solid #000}.table{border-collapse:collapse !important}.table td,.table th{background-color:#fff !important}.table-bordered th,.table-bordered td{border:1px solid #ddd !important}}@font-face{font-family:'Glyphicons Halflings';src:url('../fonts/glyphicons-halflings-regular.eot');src:url('../fonts/glyphicons-halflings-regular.eot?#iefix') format('embedded-opentype'),url('../fonts/glyphicons-halflings-regular.woff2') format('woff2'),url('../fonts/glyphicons-halflings-regular.woff') format('woff'),url('../fonts/glyphicons-halflings-regular.ttf') format('truetype'),url('../fonts/glyphicons-halflings-regular.svg#glyphicons_halflingsregular') format('svg')}.glyphicon{position:relative;top:1px;display:inline-block;font-family:'Glyphicons Halflings';font-style:normal;font-weight:normal;line-height:1;-webkit-font-smoothing:antialiased;-moz-osx-font-smoothing:grayscale}.glyphicon-asterisk:before{content:"\002a"}.glyphicon-plus:before{content:"\002b"}.glyphicon-euro:before,.glyphicon-eur:before{content:"\20ac"}.glyphicon-minus:before{content:"\2212"}.glyphicon-cloud:before{content:"\2601"}.glyphicon-envelope:before{content:"\2709"}.glyphicon-pencil:before{content:"\270f"}.glyphicon-glass:before{content:"\e001"}.glyphicon-music:before{content:"\e002"}.glyphicon-search:before{content:"\e003"}.glyphicon-heart:before{content:"\e005"}.glyphicon-star:before{content:"\e006"}.glyphicon-star-empty:before{content:"\e007"}.glyphicon-user:before{content:"\e008"}.glyphicon-film:before{content:"\e009"}.glyphicon-th-large:before{content:"\e010"}.glyphicon-th:before{content:"\e011"}.glyphicon-th-list:before{content:"\e012"}.glyphicon-ok:before{content:"\e013"}.glyphicon-remove:before{content:"\e014"}.glyphicon-zoom-in:before{content:"\e015"}.glyphicon-zoom-out:before{content:"\e016"}.glyphicon-off:before{content:"\e017"}.glyphicon-signal:before{content:"\e018"}.glyphicon-cog:before{content:"\e019"}.glyphicon-trash:before{content:"\e020"}.glyphicon-home:before{content:"\e021"}.glyphicon-file:before{content:"\e022"}.glyphicon-time:before{content:"\e023"}.glyphicon-road:before{content:"\e024"}.glyphicon-download-alt:before{content:"\e025"}.glyphicon-download:before{content:"\e026"}.glyphicon-upload:before{content:"\e027"}.glyphicon-inbox:before{content:"\e028"}.glyphicon-play-circle:before{content:"\e029"}.glyphicon-repeat:before{content:"\e030"}.glyphicon-refresh:before{content:"\e031"}.glyphicon-list-alt:before{content:"\e032"}.glyphicon-lock:before{content:"\e033"}.glyphicon-flag:before{content:"\e034"}.glyphicon-headphones:before{content:"\e035"}.glyphicon-volume-off:before{content:"\e036"}.glyphicon-volume-down:before{content:"\e037"}.glyphicon-volume-up:before{content:"\e038"}.glyphicon-qrcode:before{content:"\e039"}.glyphicon-barcode:before{content:"\e040"}.glyphicon-tag:before{content:"\e041"}.glyphicon-tags:before{content:"\e042"}.glyphicon-book:before{content:"\e043"}.glyphicon-bookmark:before{content:"\e044"}.glyphicon-print:before{content:"\e045"}.glyphicon-camera:before{content:"\e046"}.glyphicon-font:before{content:"\e047"}.glyphicon-bold:before{content:"\e048"}.glyphicon-italic:before{content:"\e049"}.glyphicon-text-height:before{content:"\e050"}.glyphicon-text-width:before{content:"\e051"}.glyphicon-align-left:before{content:"\e052"}.glyphicon-align-center:before{content:"\e053"}.glyphicon-align-right:before{content:"\e054"}.glyphicon-align-justify:before{content:"\e055"}.glyphicon-list:before{content:"\e056"}.glyphicon-indent-left:before{content:"\e057"}.glyphicon-indent-right:before{content:"\e058"}.glyphicon-facetime-video:before{content:"\e059"}.glyphicon-picture:before{content:"\e060"}.glyphicon-map-marker:before{content:"\e062"}.glyphicon-adjust:before{content:"\e063"}.glyphicon-tint:before{content:"\e064"}.glyphicon-edit:before{content:"\e065"}.glyphicon-share:before{content:"\e066"}.glyphicon-check:before{content:"\e067"}.glyphicon-move:before{content:"\e068"}.glyphicon-step-backward:before{content:"\e069"}.glyphicon-fast-backward:before{content:"\e070"}.glyphicon-backward:before{content:"\e071"}.glyphicon-play:before{content:"\e072"}.glyphicon-pause:before{content:"\e073"}.glyphicon-stop:before{content:"\e074"}.glyphicon-forward:before{content:"\e075"}.glyphicon-fast-forward:before{content:"\e076"}.glyphicon-step-forward:before{content:"\e077"}.glyphicon-eject:before{content:"\e078"}.glyphicon-chevron-left:before{content:"\e079"}.glyphicon-chevron-right:before{content:"\e080"}.glyphicon-plus-sign:before{content:"\e081"}.glyphicon-minus-sign:before{content:"\e082"}.glyphicon-remove-sign:before{content:"\e083"}.glyphicon-ok-sign:before{content:"\e084"}.glyphicon-question-sign:before{content:"\e085"}.glyphicon-info-sign:before{content:"\e086"}.glyphicon-screenshot:before{content:"\e087"}.glyphicon-remove-circle:before{content:"\e088"}.glyphicon-ok-circle:before{content:"\e089"}.glyphicon-ban-circle:before{content:"\e090"}.glyphicon-arrow-left:before{content:"\e091"}.glyphicon-arrow-right:before{content:"\e092"}.glyphicon-arrow-up:before{content:"\e093"}.glyphicon-arrow-down:before{content:"\e094"}.glyphicon-share-alt:before{content:"\e095"}.glyphicon-resize-full:before{content:"\e096"}.glyphicon-resize-small:before{content:"\e097"}.glyphicon-exclamation-sign:before{content:"\e101"}.glyphicon-gift:before{content:"\e102"}.glyphicon-leaf:before{content:"\e103"}.glyphicon-fire:before{content:"\e104"}.glyphicon-eye-open:before{content:"\e105"}.glyphicon-eye-close:before{content:"\e106"}.glyphicon-warning-sign:before{content:"\e107"}.glyphicon-plane:before{content:"\e108"}.glyphicon-calendar:before{content:"\e109"}.glyphicon-random:before{content:"\e110"}.glyphicon-comment:before{content:"\e111"}.glyphicon-magnet:before{content:"\e112"}.glyphicon-chevron-up:before{content:"\e113"}.glyphicon-chevron-down:before{content:"\e114"}.glyphicon-retweet:before{content:"\e115"}.glyphicon-shopping-cart:before{content:"\e116"}.glyphicon-folder-close:before{content:"\e117"}.glyphicon-folder-open:before{content:"\e118"}.glyphicon-resize-vertical:before{content:"\e119"}.glyphicon-resize-horizontal:before{content:"\e120"}.glyphicon-hdd:before{content:"\e121"}.glyphicon-bullhorn:before{content:"\e122"}.glyphicon-bell:before{content:"\e123"}.glyphicon-certificate:before{content:"\e124"}.glyphicon-thumbs-up:before{content:"\e125"}.glyphicon-thumbs-down:before{content:"\e126"}.glyphicon-hand-right:before{content:"\e127"}.glyphicon-hand-left:before{content:"\e128"}.glyphicon-hand-up:before{content:"\e129"}.glyphicon-hand-down:before{content:"\e130"}.glyphicon-circle-arrow-right:before{content:"\e131"}.glyphicon-circle-arrow-left:before{content:"\e132"}.glyphicon-circle-arrow-up:before{content:"\e133"}.glyphicon-circle-arrow-down:before{content:"\e134"}.glyphicon-globe:before{content:"\e135"}.glyphicon-wrench:before{content:"\e136"}.glyphicon-tasks:before{content:"\e137"}.glyphicon-filter:before{content:"\e138"}.glyphicon-briefcase:before{content:"\e139"}.glyphicon-fullscreen:before{content:"\e140"}.glyphicon-dashboard:before{content:"\e141"}.glyphicon-paperclip:before{content:"\e142"}.glyphicon-heart-empty:before{content:"\e143"}.glyphicon-link:before{content:"\e144"}.glyphicon-phone:before{content:"\e145"}.glyphicon-pushpin:before{content:"\e146"}.glyphicon-usd:before{content:"\e148"}.glyphicon-gbp:before{content:"\e149"}.glyphicon-sort:before{content:"\e150"}.glyphicon-sort-by-alphabet:before{content:"\e151"}.glyphicon-sort-by-alphabet-alt:before{content:"\e152"}.glyphicon-sort-by-order:before{content:"\e153"}.glyphicon-sort-by-order-alt:before{content:"\e154"}.glyphicon-sort-by-attributes:before{content:"\e155"}.glyphicon-sort-by-attributes-alt:before{content:"\e156"}.glyphicon-unchecked:before{content:"\e157"}.glyphicon-expand:before{content:"\e158"}.glyphicon-collapse-down:before{content:"\e159"}.glyphicon-collapse-up:before{content:"\e160"}.glyphicon-log-in:before{content:"\e161"}.glyphicon-flash:before{content:"\e162"}.glyphicon-log-out:before{content:"\e163"}.glyphicon-new-window:before{content:"\e164"}.glyphicon-record:before{content:"\e165"}.glyphicon-save:before{content:"\e166"}.glyphicon-open:before{content:"\e167"}.glyphicon-saved:before{content:"\e168"}.glyphicon-import:before{content:"\e169"}.glyphicon-export:before{content:"\e170"}.glyphicon-send:before{content:"\e171"}.glyphicon-floppy-disk:before{content:"\e172"}.glyphicon-floppy-saved:before{content:"\e173"}.glyphicon-floppy-remove:before{content:"\e174"}.glyphicon-floppy-save:before{content:"\e175"}.glyphicon-floppy-open:before{content:"\e176"}.glyphicon-credit-card:before{content:"\e177"}.glyphicon-transfer:before{content:"\e178"}.glyphicon-cutlery:before{content:"\e179"}.glyphicon-header:before{content:"\e180"}.glyphicon-compressed:before{content:"\e181"}.glyphicon-earphone:before{content:"\e182"}.glyphicon-phone-alt:before{content:"\e183"}.glyphicon-tower:before{content:"\e184"}.glyphicon-stats:before{content:"\e185"}.glyphicon-sd-video:before{content:"\e186"}.glyphicon-hd-video:before{content:"\e187"}.glyphicon-subtitles:before{content:"\e188"}.glyphicon-sound-stereo:before{content:"\e189"}.glyphicon-sound-dolby:before{content:"\e190"}.glyphicon-sound-5-1:before{content:"\e191"}.glyphicon-sound-6-1:before{content:"\e192"}.glyphicon-sound-7-1:before{content:"\e193"}.glyphicon-copyright-mark:before{content:"\e194"}.glyphicon-registration-mark:before{content:"\e195"}.glyphicon-cloud-download:before{content:"\e197"}.glyphicon-cloud-upload:before{content:"\e198"}.glyphicon-tree-conifer:before{content:"\e199"}.glyphicon-tree-deciduous:before{content:"\e200"}.glyphicon-cd:before{content:"\e201"}.glyphicon-save-file:before{content:"\e202"}.glyphicon-open-file:before{content:"\e203"}.glyphicon-level-up:before{content:"\e204"}.glyphicon-copy:before{content:"\e205"}.glyphicon-paste:before{content:"\e206"}.glyphicon-alert:before{content:"\e209"}.glyphicon-equalizer:before{content:"\e210"}.glyphicon-king:before{content:"\e211"}.glyphicon-queen:before{content:"\e212"}.glyphicon-pawn:before{content:"\e213"}.glyphicon-bishop:before{content:"\e214"}.glyphicon-knight:before{content:"\e215"}.glyphicon-baby-formula:before{content:"\e216"}.glyphicon-tent:before{content:"\26fa"}.glyphicon-blackboard:before{content:"\e218"}.glyphicon-bed:before{content:"\e219"}.glyphicon-apple:before{content:"\f8ff"}.glyphicon-erase:before{content:"\e221"}.glyphicon-hourglass:before{content:"\231b"}.glyphicon-lamp:before{content:"\e223"}.glyphicon-duplicate:before{content:"\e224"}.glyphicon-piggy-bank:before{content:"\e225"}.glyphicon-scissors:before{content:"\e226"}.glyphicon-bitcoin:before{content:"\e227"}.glyphicon-btc:before{content:"\e227"}.glyphicon-xbt:before{content:"\e227"}.glyphicon-yen:before{content:"\00a5"}.glyphicon-jpy:before{content:"\00a5"}.glyphicon-ruble:before{content:"\20bd"}.glyphicon-rub:before{content:"\20bd"}.glyphicon-scale:before{content:"\e230"}.glyphicon-ice-lolly:before{content:"\e231"}.glyphicon-ice-lolly-tasted:before{content:"\e232"}.glyphicon-education:before{content:"\e233"}.glyphicon-option-horizontal:before{content:"\e234"}.glyphicon-option-vertical:before{content:"\e235"}.glyphicon-menu-hamburger:before{content:"\e236"}.glyphicon-modal-window:before{content:"\e237"}.glyphicon-oil:before{content:"\e238"}.glyphicon-grain:before{content:"\e239"}.glyphicon-sunglasses:before{content:"\e240"}.glyphicon-text-size:before{content:"\e241"}.glyphicon-text-color:before{content:"\e242"}.glyphicon-text-background:before{content:"\e243"}.glyphicon-object-align-top:before{content:"\e244"}.glyphicon-object-align-bottom:before{content:"\e245"}.glyphicon-object-align-horizontal:before{content:"\e246"}.glyphicon-object-align-left:before{content:"\e247"}.glyphicon-object-align-vertical:before{content:"\e248"}.glyphicon-object-align-right:before{content:"\e249"}.glyphicon-triangle-right:before{content:"\e250"}.glyphicon-triangle-left:before{content:"\e251"}.glyphicon-triangle-bottom:before{content:"\e252"}.glyphicon-triangle-top:before{content:"\e253"}.glyphicon-console:before{content:"\e254"}.glyphicon-superscript:before{content:"\e255"}.glyphicon-subscript:before{content:"\e256"}.glyphicon-menu-left:before{content:"\e257"}.glyphicon-menu-right:before{content:"\e258"}.glyphicon-menu-down:before{content:"\e259"}.glyphicon-menu-up:before{content:"\e260"}*{-webkit-box-sizing:border-box;-moz-box-sizing:border-box;box-sizing:border-box}*:before,*:after{-webkit-box-sizing:border-box;-moz-box-sizing:border-box;box-sizing:border-box}html{font-size:10px;-webkit-tap-highlight-color:rgba(0,0,0,0)}body{font-family:"Helvetica Neue",Helvetica,Arial,sans-serif;font-size:14px;line-height:1.42857143;color:#555555;background-color:#ffffff}input,button,select,textarea{font-family:inherit;font-size:inherit;line-height:inherit}a{color:#2fa4e7;text-decoration:none}a:hover,a:focus{color:#157ab5;text-decoration:underline}a:focus{outline:5px auto -webkit-focus-ring-color;outline-offset:-2px}figure{margin:0}img{vertical-align:middle}.img-responsive,.thumbnail>img,.thumbnail a>img,.carousel-inner>.item>img,.carousel-inner>.item>a>img{display:block;max-width:100%;height:auto}.img-rounded{border-radius:6px}.img-thumbnail{padding:4px;line-height:1.42857143;background-color:#ffffff;border:1px solid #dddddd;border-radius:4px;-webkit-transition:all .2s ease-in-out;-o-transition:all .2s ease-in-out;transition:all .2s ease-in-out;display:inline-block;max-width:100%;height:auto}.img-circle{border-radius:50%}hr{margin-top:20px;margin-bottom:20px;border:0;border-top:1px solid #eeeeee}.sr-only{position:absolute;width:1px;height:1px;margin:-1px;padding:0;overflow:hidden;clip:rect(0, 0, 0, 0);border:0}.sr-only-focusable:active,.sr-only-focusable:focus{position:static;width:auto;height:auto;margin:0;overflow:visible;clip:auto}[role="button"]{cursor:pointer}h1,h2,h3,h4,h5,h6,.h1,.h2,.h3,.h4,.h5,.h6{font-family:"Helvetica Neue",Helvetica,Arial,sans-serif;font-weight:500;line-height:1.2;color:#317eac}h1 small,h2 small,h3 small,h4 small,h5 small,h6 small,.h1 small,.h2 small,.h3 small,.h4 small,.h5 small,.h6 small,h1 .small,h2 .small,h3 .small,h4 .small,h5 .small,h6 .small,.h1 .small,.h2 .small,.h3 .small,.h4 .small,.h5 .small,.h6 .small{font-weight:normal;line-height:1;color:#999999}h1,.h1,h2,.h2,h3,.h3{margin-top:20px;margin-bottom:10px}h1 small,.h1 small,h2 small,.h2 small,h3 small,.h3 small,h1 .small,.h1 .small,h2 .small,.h2 .small,h3 .small,.h3 .small{font-size:65%}h4,.h4,h5,.h5,h6,.h6{margin-top:10px;margin-bottom:10px}h4 small,.h4 small,h5 small,.h5 small,h6 small,.h6 small,h4 .small,.h4 .small,h5 .small,.h5 .small,h6 .small,.h6 .small{font-size:75%}h1,.h1{font-size:36px}h2,.h2{font-size:30px}h3,.h3{font-size:24px}h4,.h4{font-size:18px}h5,.h5{font-size:14px}h6,.h6{font-size:12px}p{margin:0 0 10px}.lead{margin-bottom:20px;font-size:16px;font-weight:300;line-height:1.4}@media (min-width:768px){.lead{font-size:21px}}small,.small{font-size:85%}mark,.mark{background-color:#fcf8e3;padding:.2em}.text-left{text-align:left}.text-right{text-align:right}.text-center{text-align:center}.text-justify{text-align:justify}.text-nowrap{white-space:nowrap}.text-lowercase{text-transform:lowercase}.text-uppercase{text-transform:uppercase}.text-capitalize{text-transform:capitalize}.text-muted{color:#999999}.text-primary{color:#2fa4e7}a.text-primary:hover,a.text-primary:focus{color:#178acc}.text-success{color:#468847}a.text-success:hover,a.text-success:focus{color:#356635}.text-info{color:#3a87ad}a.text-info:hover,a.text-info:focus{color:#2d6987}.text-warning{color:#c09853}a.text-warning:hover,a.text-warning:focus{color:#a47e3c}.text-danger{color:#b94a48}a.text-danger:hover,a.text-danger:focus{color:#953b39}.bg-primary{color:#fff;background-color:#2fa4e7}a.bg-primary:hover,a.bg-primary:focus{background-color:#178acc}.bg-success{background-color:#dff0d8}a.bg-success:hover,a.bg-success:focus{background-color:#c1e2b3}.bg-info{background-color:#d9edf7}a.bg-info:hover,a.bg-info:focus{background-color:#afd9ee}.bg-warning{background-color:#fcf8e3}a.bg-warning:hover,a.bg-warning:focus{background-color:#f7ecb5}.bg-danger{background-color:#f2dede}a.bg-danger:hover,a.bg-danger:focus{background-color:#e4b9b9}.page-header{padding-bottom:9px;margin:40px 0 20px;border-bottom:1px solid #eeeeee}ul,ol{margin-top:0;margin-bottom:10px}ul ul,ol ul,ul ol,ol ol{margin-bottom:0}.list-unstyled{padding-left:0;list-style:none}.list-inline{padding-left:0;list-style:none;margin-left:-5px}.list-inline>li{display:inline-block;padding-left:5px;padding-right:5px}dl{margin-top:0;margin-bottom:20px}dt,dd{line-height:1.42857143}dt{font-weight:bold}dd{margin-left:0}@media (min-width:768px){.dl-horizontal dt{float:left;width:160px;clear:left;text-align:right;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.dl-horizontal dd{margin-left:180px}}abbr[title],abbr[data-original-title]{cursor:help;border-bottom:1px dotted #999999}.initialism{font-size:90%;text-transform:uppercase}blockquote{padding:10px 20px;margin:0 0 20px;font-size:17.5px;border-left:5px solid #eeeeee}blockquote p:last-child,blockquote ul:last-child,blockquote ol:last-child{margin-bottom:0}blockquote footer,blockquote small,blockquote .small{display:block;font-size:80%;line-height:1.42857143;color:#999999}blockquote footer:before,blockquote small:before,blockquote .small:before{content:'\2014 \00A0'}.blockquote-reverse,blockquote.pull-right{padding-right:15px;padding-left:0;border-right:5px solid #eeeeee;border-left:0;text-align:right}.blockquote-reverse footer:before,blockquote.pull-right footer:before,.blockquote-reverse small:before,blockquote.pull-right small:before,.blockquote-reverse .small:before,blockquote.pull-right .small:before{content:''}.blockquote-reverse footer:after,blockquote.pull-right footer:after,.blockquote-reverse small:after,blockquote.pull-right small:after,.blockquote-reverse .small:after,blockquote.pull-right .small:after{content:'\00A0 \2014'}address{margin-bottom:20px;font-style:normal;line-height:1.42857143}code,kbd,pre,samp{font-family:Menlo,Monaco,Consolas,"Courier New",monospace}code{padding:2px 4px;font-size:90%;color:#c7254e;background-color:#f9f2f4;border-radius:4px}kbd{padding:2px 4px;font-size:90%;color:#ffffff;background-color:#333333;border-radius:3px;-webkit-box-shadow:inset 0 -1px 0 rgba(0,0,0,0.25);box-shadow:inset 0 -1px 0 rgba(0,0,0,0.25)}kbd kbd{padding:0;font-size:100%;font-weight:bold;-webkit-box-shadow:none;box-shadow:none}pre{display:block;padding:9.5px;margin:0 0 10px;font-size:13px;line-height:1.42857143;word-break:break-all;word-wrap:break-word;color:#333333;background-color:#f5f5f5;border:1px solid #cccccc;border-radius:4px}pre code{padding:0;font-size:inherit;color:inherit;white-space:pre-wrap;background-color:transparent;border-radius:0}.pre-scrollable{max-height:340px;overflow-y:scroll}.container{margin-right:auto;margin-left:auto;padding-left:15px;padding-right:15px}@media (min-width:768px){.container{width:750px}}@media (min-width:992px){.container{width:970px}}@media (min-width:1200px){.container{width:1170px}}.container-fluid{margin-right:auto;margin-left:auto;padding-left:15px;padding-right:15px}.row{margin-left:-15px;margin-right:-15px}.col-xs-1,.col-sm-1,.col-md-1,.col-lg-1,.col-xs-2,.col-sm-2,.col-md-2,.col-lg-2,.col-xs-3,.col-sm-3,.col-md-3,.col-lg-3,.col-xs-4,.col-sm-4,.col-md-4,.col-lg-4,.col-xs-5,.col-sm-5,.col-md-5,.col-lg-5,.col-xs-6,.col-sm-6,.col-md-6,.col-lg-6,.col-xs-7,.col-sm-7,.col-md-7,.col-lg-7,.col-xs-8,.col-sm-8,.col-md-8,.col-lg-8,.col-xs-9,.col-sm-9,.col-md-9,.col-lg-9,.col-xs-10,.col-sm-10,.col-md-10,.col-lg-10,.col-xs-11,.col-sm-11,.col-md-11,.col-lg-11,.col-xs-12,.col-sm-12,.col-md-12,.col-lg-12{position:relative;min-height:1px;padding-left:15px;padding-right:15px}.col-xs-1,.col-xs-2,.col-xs-3,.col-xs-4,.col-xs-5,.col-xs-6,.col-xs-7,.col-xs-8,.col-xs-9,.col-xs-10,.col-xs-11,.col-xs-12{float:left}.col-xs-12{width:100%}.col-xs-11{width:91.66666667%}.col-xs-10{width:83.33333333%}.col-xs-9{width:75%}.col-xs-8{width:66.66666667%}.col-xs-7{width:58.33333333%}.col-xs-6{width:50%}.col-xs-5{width:41.66666667%}.col-xs-4{width:33.33333333%}.col-xs-3{width:25%}.col-xs-2{width:16.66666667%}.col-xs-1{width:8.33333333%}.col-xs-pull-12{right:100%}.col-xs-pull-11{right:91.66666667%}.col-xs-pull-10{right:83.33333333%}.col-xs-pull-9{right:75%}.col-xs-pull-8{right:66.66666667%}.col-xs-pull-7{right:58.33333333%}.col-xs-pull-6{right:50%}.col-xs-pull-5{right:41.66666667%}.col-xs-pull-4{right:33.33333333%}.col-xs-pull-3{right:25%}.col-xs-pull-2{right:16.66666667%}.col-xs-pull-1{right:8.33333333%}.col-xs-pull-0{right:auto}.col-xs-push-12{left:100%}.col-xs-push-11{left:91.66666667%}.col-xs-push-10{left:83.33333333%}.col-xs-push-9{left:75%}.col-xs-push-8{left:66.66666667%}.col-xs-push-7{left:58.33333333%}.col-xs-push-6{left:50%}.col-xs-push-5{left:41.66666667%}.col-xs-push-4{left:33.33333333%}.col-xs-push-3{left:25%}.col-xs-push-2{left:16.66666667%}.col-xs-push-1{left:8.33333333%}.col-xs-push-0{left:auto}.col-xs-offset-12{margin-left:100%}.col-xs-offset-11{margin-left:91.66666667%}.col-xs-offset-10{margin-left:83.33333333%}.col-xs-offset-9{margin-left:75%}.col-xs-offset-8{margin-left:66.66666667%}.col-xs-offset-7{margin-left:58.33333333%}.col-xs-offset-6{margin-left:50%}.col-xs-offset-5{margin-left:41.66666667%}.col-xs-offset-4{margin-left:33.33333333%}.col-xs-offset-3{margin-left:25%}.col-xs-offset-2{margin-left:16.66666667%}.col-xs-offset-1{margin-left:8.33333333%}.col-xs-offset-0{margin-left:0%}@media (min-width:768px){.col-sm-1,.col-sm-2,.col-sm-3,.col-sm-4,.col-sm-5,.col-sm-6,.col-sm-7,.col-sm-8,.col-sm-9,.col-sm-10,.col-sm-11,.col-sm-12{float:left}.col-sm-12{width:100%}.col-sm-11{width:91.66666667%}.col-sm-10{width:83.33333333%}.col-sm-9{width:75%}.col-sm-8{width:66.66666667%}.col-sm-7{width:58.33333333%}.col-sm-6{width:50%}.col-sm-5{width:41.66666667%}.col-sm-4{width:33.33333333%}.col-sm-3{width:25%}.col-sm-2{width:16.66666667%}.col-sm-1{width:8.33333333%}.col-sm-pull-12{right:100%}.col-sm-pull-11{right:91.66666667%}.col-sm-pull-10{right:83.33333333%}.col-sm-pull-9{right:75%}.col-sm-pull-8{right:66.66666667%}.col-sm-pull-7{right:58.33333333%}.col-sm-pull-6{right:50%}.col-sm-pull-5{right:41.66666667%}.col-sm-pull-4{right:33.33333333%}.col-sm-pull-3{right:25%}.col-sm-pull-2{right:16.66666667%}.col-sm-pull-1{right:8.33333333%}.col-sm-pull-0{right:auto}.col-sm-push-12{left:100%}.col-sm-push-11{left:91.66666667%}.col-sm-push-10{left:83.33333333%}.col-sm-push-9{left:75%}.col-sm-push-8{left:66.66666667%}.col-sm-push-7{left:58.33333333%}.col-sm-push-6{left:50%}.col-sm-push-5{left:41.66666667%}.col-sm-push-4{left:33.33333333%}.col-sm-push-3{left:25%}.col-sm-push-2{left:16.66666667%}.col-sm-push-1{left:8.33333333%}.col-sm-push-0{left:auto}.col-sm-offset-12{margin-left:100%}.col-sm-offset-11{margin-left:91.66666667%}.col-sm-offset-10{margin-left:83.33333333%}.col-sm-offset-9{margin-left:75%}.col-sm-offset-8{margin-left:66.66666667%}.col-sm-offset-7{margin-left:58.33333333%}.col-sm-offset-6{margin-left:50%}.col-sm-offset-5{margin-left:41.66666667%}.col-sm-offset-4{margin-left:33.33333333%}.col-sm-offset-3{margin-left:25%}.col-sm-offset-2{margin-left:16.66666667%}.col-sm-offset-1{margin-left:8.33333333%}.col-sm-offset-0{margin-left:0%}}@media (min-width:992px){.col-md-1,.col-md-2,.col-md-3,.col-md-4,.col-md-5,.col-md-6,.col-md-7,.col-md-8,.col-md-9,.col-md-10,.col-md-11,.col-md-12{float:left}.col-md-12{width:100%}.col-md-11{width:91.66666667%}.col-md-10{width:83.33333333%}.col-md-9{width:75%}.col-md-8{width:66.66666667%}.col-md-7{width:58.33333333%}.col-md-6{width:50%}.col-md-5{width:41.66666667%}.col-md-4{width:33.33333333%}.col-md-3{width:25%}.col-md-2{width:16.66666667%}.col-md-1{width:8.33333333%}.col-md-pull-12{right:100%}.col-md-pull-11{right:91.66666667%}.col-md-pull-10{right:83.33333333%}.col-md-pull-9{right:75%}.col-md-pull-8{right:66.66666667%}.col-md-pull-7{right:58.33333333%}.col-md-pull-6{right:50%}.col-md-pull-5{right:41.66666667%}.col-md-pull-4{right:33.33333333%}.col-md-pull-3{right:25%}.col-md-pull-2{right:16.66666667%}.col-md-pull-1{right:8.33333333%}.col-md-pull-0{right:auto}.col-md-push-12{left:100%}.col-md-push-11{left:91.66666667%}.col-md-push-10{left:83.33333333%}.col-md-push-9{left:75%}.col-md-push-8{left:66.66666667%}.col-md-push-7{left:58.33333333%}.col-md-push-6{left:50%}.col-md-push-5{left:41.66666667%}.col-md-push-4{left:33.33333333%}.col-md-push-3{left:25%}.col-md-push-2{left:16.66666667%}.col-md-push-1{left:8.33333333%}.col-md-push-0{left:auto}.col-md-offset-12{margin-left:100%}.col-md-offset-11{margin-left:91.66666667%}.col-md-offset-10{margin-left:83.33333333%}.col-md-offset-9{margin-left:75%}.col-md-offset-8{margin-left:66.66666667%}.col-md-offset-7{margin-left:58.33333333%}.col-md-offset-6{margin-left:50%}.col-md-offset-5{margin-left:41.66666667%}.col-md-offset-4{margin-left:33.33333333%}.col-md-offset-3{margin-left:25%}.col-md-offset-2{margin-left:16.66666667%}.col-md-offset-1{margin-left:8.33333333%}.col-md-offset-0{margin-left:0%}}@media (min-width:1200px){.col-lg-1,.col-lg-2,.col-lg-3,.col-lg-4,.col-lg-5,.col-lg-6,.col-lg-7,.col-lg-8,.col-lg-9,.col-lg-10,.col-lg-11,.col-lg-12{float:left}.col-lg-12{width:100%}.col-lg-11{width:91.66666667%}.col-lg-10{width:83.33333333%}.col-lg-9{width:75%}.col-lg-8{width:66.66666667%}.col-lg-7{width:58.33333333%}.col-lg-6{width:50%}.col-lg-5{width:41.66666667%}.col-lg-4{width:33.33333333%}.col-lg-3{width:25%}.col-lg-2{width:16.66666667%}.col-lg-1{width:8.33333333%}.col-lg-pull-12{right:100%}.col-lg-pull-11{right:91.66666667%}.col-lg-pull-10{right:83.33333333%}.col-lg-pull-9{right:75%}.col-lg-pull-8{right:66.66666667%}.col-lg-pull-7{right:58.33333333%}.col-lg-pull-6{right:50%}.col-lg-pull-5{right:41.66666667%}.col-lg-pull-4{right:33.33333333%}.col-lg-pull-3{right:25%}.col-lg-pull-2{right:16.66666667%}.col-lg-pull-1{right:8.33333333%}.col-lg-pull-0{right:auto}.col-lg-push-12{left:100%}.col-lg-push-11{left:91.66666667%}.col-lg-push-10{left:83.33333333%}.col-lg-push-9{left:75%}.col-lg-push-8{left:66.66666667%}.col-lg-push-7{left:58.33333333%}.col-lg-push-6{left:50%}.col-lg-push-5{left:41.66666667%}.col-lg-push-4{left:33.33333333%}.col-lg-push-3{left:25%}.col-lg-push-2{left:16.66666667%}.col-lg-push-1{left:8.33333333%}.col-lg-push-0{left:auto}.col-lg-offset-12{margin-left:100%}.col-lg-offset-11{margin-left:91.66666667%}.col-lg-offset-10{margin-left:83.33333333%}.col-lg-offset-9{margin-left:75%}.col-lg-offset-8{margin-left:66.66666667%}.col-lg-offset-7{margin-left:58.33333333%}.col-lg-offset-6{margin-left:50%}.col-lg-offset-5{margin-left:41.66666667%}.col-lg-offset-4{margin-left:33.33333333%}.col-lg-offset-3{margin-left:25%}.col-lg-offset-2{margin-left:16.66666667%}.col-lg-offset-1{margin-left:8.33333333%}.col-lg-offset-0{margin-left:0%}}table{background-color:transparent}caption{padding-top:8px;padding-bottom:8px;color:#999999;text-align:left}th{text-align:left}.table{width:100%;max-width:100%;margin-bottom:20px}.table>thead>tr>th,.table>tbody>tr>th,.table>tfoot>tr>th,.table>thead>tr>td,.table>tbody>tr>td,.table>tfoot>tr>td{padding:8px;line-height:1.42857143;vertical-align:top;border-top:1px solid #dddddd}.table>thead>tr>th{vertical-align:bottom;border-bottom:2px solid #dddddd}.table>caption+thead>tr:first-child>th,.table>colgroup+thead>tr:first-child>th,.table>thead:first-child>tr:first-child>th,.table>caption+thead>tr:first-child>td,.table>colgroup+thead>tr:first-child>td,.table>thead:first-child>tr:first-child>td{border-top:0}.table>tbody+tbody{border-top:2px solid #dddddd}.table .table{background-color:#ffffff}.table-condensed>thead>tr>th,.table-condensed>tbody>tr>th,.table-condensed>tfoot>tr>th,.table-condensed>thead>tr>td,.table-condensed>tbody>tr>td,.table-condensed>tfoot>tr>td{padding:5px}.table-bordered{border:1px solid #dddddd}.table-bordered>thead>tr>th,.table-bordered>tbody>tr>th,.table-bordered>tfoot>tr>th,.table-bordered>thead>tr>td,.table-bordered>tbody>tr>td,.table-bordered>tfoot>tr>td{border:1px solid #dddddd}.table-bordered>thead>tr>th,.table-bordered>thead>tr>td{border-bottom-width:2px}.table-striped>tbody>tr:nth-of-type(odd){background-color:#f9f9f9}.table-hover>tbody>tr:hover{background-color:#f5f5f5}table col[class*="col-"]{position:static;float:none;display:table-column}table td[class*="col-"],table th[class*="col-"]{position:static;float:none;display:table-cell}.table>thead>tr>td.active,.table>tbody>tr>td.active,.table>tfoot>tr>td.active,.table>thead>tr>th.active,.table>tbody>tr>th.active,.table>tfoot>tr>th.active,.table>thead>tr.active>td,.table>tbody>tr.active>td,.table>tfoot>tr.active>td,.table>thead>tr.active>th,.table>tbody>tr.active>th,.table>tfoot>tr.active>th{background-color:#f5f5f5}.table-hover>tbody>tr>td.active:hover,.table-hover>tbody>tr>th.active:hover,.table-hover>tbody>tr.active:hover>td,.table-hover>tbody>tr:hover>.active,.table-hover>tbody>tr.active:hover>th{background-color:#e8e8e8}.table>thead>tr>td.success,.table>tbody>tr>td.success,.table>tfoot>tr>td.success,.table>thead>tr>th.success,.table>tbody>tr>th.success,.table>tfoot>tr>th.success,.table>thead>tr.success>td,.table>tbody>tr.success>td,.table>tfoot>tr.success>td,.table>thead>tr.success>th,.table>tbody>tr.success>th,.table>tfoot>tr.success>th{background-color:#dff0d8}.table-hover>tbody>tr>td.success:hover,.table-hover>tbody>tr>th.success:hover,.table-hover>tbody>tr.success:hover>td,.table-hover>tbody>tr:hover>.success,.table-hover>tbody>tr.success:hover>th{background-color:#d0e9c6}.table>thead>tr>td.info,.table>tbody>tr>td.info,.table>tfoot>tr>td.info,.table>thead>tr>th.info,.table>tbody>tr>th.info,.table>tfoot>tr>th.info,.table>thead>tr.info>td,.table>tbody>tr.info>td,.table>tfoot>tr.info>td,.table>thead>tr.info>th,.table>tbody>tr.info>th,.table>tfoot>tr.info>th{background-color:#d9edf7}.table-hover>tbody>tr>td.info:hover,.table-hover>tbody>tr>th.info:hover,.table-hover>tbody>tr.info:hover>td,.table-hover>tbody>tr:hover>.info,.table-hover>tbody>tr.info:hover>th{background-color:#c4e3f3}.table>thead>tr>td.warning,.table>tbody>tr>td.warning,.table>tfoot>tr>td.warning,.table>thead>tr>th.warning,.table>tbody>tr>th.warning,.table>tfoot>tr>th.warning,.table>thead>tr.warning>td,.table>tbody>tr.warning>td,.table>tfoot>tr.warning>td,.table>thead>tr.warning>th,.table>tbody>tr.warning>th,.table>tfoot>tr.warning>th{background-color:#fcf8e3}.table-hover>tbody>tr>td.warning:hover,.table-hover>tbody>tr>th.warning:hover,.table-hover>tbody>tr.warning:hover>td,.table-hover>tbody>tr:hover>.warning,.table-hover>tbody>tr.warning:hover>th{background-color:#faf2cc}.table>thead>tr>td.danger,.table>tbody>tr>td.danger,.table>tfoot>tr>td.danger,.table>thead>tr>th.danger,.table>tbody>tr>th.danger,.table>tfoot>tr>th.danger,.table>thead>tr.danger>td,.table>tbody>tr.danger>td,.table>tfoot>tr.danger>td,.table>thead>tr.danger>th,.table>tbody>tr.danger>th,.table>tfoot>tr.danger>th{background-color:#f2dede}.table-hover>tbody>tr>td.danger:hover,.table-hover>tbody>tr>th.danger:hover,.table-hover>tbody>tr.danger:hover>td,.table-hover>tbody>tr:hover>.danger,.table-hover>tbody>tr.danger:hover>th{background-color:#ebcccc}.table-responsive{overflow-x:auto;min-height:0.01%}@media screen and (max-width:767px){.table-responsive{width:100%;margin-bottom:15px;overflow-y:hidden;-ms-overflow-style:-ms-autohiding-scrollbar;border:1px solid #dddddd}.table-responsive>.table{margin-bottom:0}.table-responsive>.table>thead>tr>th,.table-responsive>.table>tbody>tr>th,.table-responsive>.table>tfoot>tr>th,.table-responsive>.table>thead>tr>td,.table-responsive>.table>tbody>tr>td,.table-responsive>.table>tfoot>tr>td{white-space:nowrap}.table-responsive>.table-bordered{border:0}.table-responsive>.table-bordered>thead>tr>th:first-child,.table-responsive>.table-bordered>tbody>tr>th:first-child,.table-responsive>.table-bordered>tfoot>tr>th:first-child,.table-responsive>.table-bordered>thead>tr>td:first-child,.table-responsive>.table-bordered>tbody>tr>td:first-child,.table-responsive>.table-bordered>tfoot>tr>td:first-child{border-left:0}.table-responsive>.table-bordered>thead>tr>th:last-child,.table-responsive>.table-bordered>tbody>tr>th:last-child,.table-responsive>.table-bordered>tfoot>tr>th:last-child,.table-responsive>.table-bordered>thead>tr>td:last-child,.table-responsive>.table-bordered>tbody>tr>td:last-child,.table-responsive>.table-bordered>tfoot>tr>td:last-child{border-right:0}.table-responsive>.table-bordered>tbody>tr:last-child>th,.table-responsive>.table-bordered>tfoot>tr:last-child>th,.table-responsive>.table-bordered>tbody>tr:last-child>td,.table-responsive>.table-bordered>tfoot>tr:last-child>td{border-bottom:0}}fieldset{padding:0;margin:0;border:0;min-width:0}legend{display:block;width:100%;padding:0;margin-bottom:20px;font-size:21px;line-height:inherit;color:#555555;border:0;border-bottom:1px solid #e5e5e5}label{display:inline-block;max-width:100%;margin-bottom:5px;font-weight:bold}input[type="search"]{-webkit-box-sizing:border-box;-moz-box-sizing:border-box;box-sizing:border-box}input[type="radio"],input[type="checkbox"]{margin:4px 0 0;margin-top:1px \9;line-height:normal}input[type="file"]{display:block}input[type="range"]{display:block;width:100%}select[multiple],select[size]{height:auto}input[type="file"]:focus,input[type="radio"]:focus,input[type="checkbox"]:focus{outline:5px auto -webkit-focus-ring-color;outline-offset:-2px}output{display:block;padding-top:9px;font-size:14px;line-height:1.42857143;color:#555555}.form-control{display:block;width:100%;height:38px;padding:8px 12px;font-size:14px;line-height:1.42857143;color:#555555;background-color:#ffffff;background-image:none;border:1px solid #cccccc;border-radius:4px;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.075);box-shadow:inset 0 1px 1px rgba(0,0,0,0.075);-webkit-transition:border-color ease-in-out .15s,-webkit-box-shadow ease-in-out .15s;-o-transition:border-color ease-in-out .15s,box-shadow ease-in-out .15s;transition:border-color ease-in-out .15s,box-shadow ease-in-out .15s}.form-control:focus{border-color:#66afe9;outline:0;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.075),0 0 8px rgba(102,175,233,0.6);box-shadow:inset 0 1px 1px rgba(0,0,0,0.075),0 0 8px rgba(102,175,233,0.6)}.form-control::-moz-placeholder{color:#999999;opacity:1}.form-control:-ms-input-placeholder{color:#999999}.form-control::-webkit-input-placeholder{color:#999999}.form-control::-ms-expand{border:0;background-color:transparent}.form-control[disabled],.form-control[readonly],fieldset[disabled] .form-control{background-color:#eeeeee;opacity:1}.form-control[disabled],fieldset[disabled] .form-control{cursor:not-allowed}textarea.form-control{height:auto}input[type="search"]{-webkit-appearance:none}@media screen and (-webkit-min-device-pixel-ratio:0){input[type="date"].form-control,input[type="time"].form-control,input[type="datetime-local"].form-control,input[type="month"].form-control{line-height:38px}input[type="date"].input-sm,input[type="time"].input-sm,input[type="datetime-local"].input-sm,input[type="month"].input-sm,.input-group-sm input[type="date"],.input-group-sm input[type="time"],.input-group-sm input[type="datetime-local"],.input-group-sm input[type="month"]{line-height:30px}input[type="date"].input-lg,input[type="time"].input-lg,input[type="datetime-local"].input-lg,input[type="month"].input-lg,.input-group-lg input[type="date"],.input-group-lg input[type="time"],.input-group-lg input[type="datetime-local"],.input-group-lg input[type="month"]{line-height:54px}}.form-group{margin-bottom:15px}.radio,.checkbox{position:relative;display:block;margin-top:10px;margin-bottom:10px}.radio label,.checkbox label{min-height:20px;padding-left:20px;margin-bottom:0;font-weight:normal;cursor:pointer}.radio input[type="radio"],.radio-inline input[type="radio"],.checkbox input[type="checkbox"],.checkbox-inline input[type="checkbox"]{position:absolute;margin-left:-20px;margin-top:4px \9}.radio+.radio,.checkbox+.checkbox{margin-top:-5px}.radio-inline,.checkbox-inline{position:relative;display:inline-block;padding-left:20px;margin-bottom:0;vertical-align:middle;font-weight:normal;cursor:pointer}.radio-inline+.radio-inline,.checkbox-inline+.checkbox-inline{margin-top:0;margin-left:10px}input[type="radio"][disabled],input[type="checkbox"][disabled],input[type="radio"].disabled,input[type="checkbox"].disabled,fieldset[disabled] input[type="radio"],fieldset[disabled] input[type="checkbox"]{cursor:not-allowed}.radio-inline.disabled,.checkbox-inline.disabled,fieldset[disabled] .radio-inline,fieldset[disabled] .checkbox-inline{cursor:not-allowed}.radio.disabled label,.checkbox.disabled label,fieldset[disabled] .radio label,fieldset[disabled] .checkbox label{cursor:not-allowed}.form-control-static{padding-top:9px;padding-bottom:9px;margin-bottom:0;min-height:34px}.form-control-static.input-lg,.form-control-static.input-sm{padding-left:0;padding-right:0}.input-sm{height:30px;padding:5px 10px;font-size:12px;line-height:1.5;border-radius:3px}select.input-sm{height:30px;line-height:30px}textarea.input-sm,select[multiple].input-sm{height:auto}.form-group-sm .form-control{height:30px;padding:5px 10px;font-size:12px;line-height:1.5;border-radius:3px}.form-group-sm select.form-control{height:30px;line-height:30px}.form-group-sm textarea.form-control,.form-group-sm select[multiple].form-control{height:auto}.form-group-sm .form-control-static{height:30px;min-height:32px;padding:6px 10px;font-size:12px;line-height:1.5}.input-lg{height:54px;padding:14px 16px;font-size:18px;line-height:1.3333333;border-radius:6px}select.input-lg{height:54px;line-height:54px}textarea.input-lg,select[multiple].input-lg{height:auto}.form-group-lg .form-control{height:54px;padding:14px 16px;font-size:18px;line-height:1.3333333;border-radius:6px}.form-group-lg select.form-control{height:54px;line-height:54px}.form-group-lg textarea.form-control,.form-group-lg select[multiple].form-control{height:auto}.form-group-lg .form-control-static{height:54px;min-height:38px;padding:15px 16px;font-size:18px;line-height:1.3333333}.has-feedback{position:relative}.has-feedback .form-control{padding-right:47.5px}.form-control-feedback{position:absolute;top:0;right:0;z-index:2;display:block;width:38px;height:38px;line-height:38px;text-align:center;pointer-events:none}.input-lg+.form-control-feedback,.input-group-lg+.form-control-feedback,.form-group-lg .form-control+.form-control-feedback{width:54px;height:54px;line-height:54px}.input-sm+.form-control-feedback,.input-group-sm+.form-control-feedback,.form-group-sm .form-control+.form-control-feedback{width:30px;height:30px;line-height:30px}.has-success .help-block,.has-success .control-label,.has-success .radio,.has-success .checkbox,.has-success .radio-inline,.has-success .checkbox-inline,.has-success.radio label,.has-success.checkbox label,.has-success.radio-inline label,.has-success.checkbox-inline label{color:#468847}.has-success .form-control{border-color:#468847;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.075);box-shadow:inset 0 1px 1px rgba(0,0,0,0.075)}.has-success .form-control:focus{border-color:#356635;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.075),0 0 6px #7aba7b;box-shadow:inset 0 1px 1px rgba(0,0,0,0.075),0 0 6px #7aba7b}.has-success .input-group-addon{color:#468847;border-color:#468847;background-color:#dff0d8}.has-success .form-control-feedback{color:#468847}.has-warning .help-block,.has-warning .control-label,.has-warning .radio,.has-warning .checkbox,.has-warning .radio-inline,.has-warning .checkbox-inline,.has-warning.radio label,.has-warning.checkbox label,.has-warning.radio-inline label,.has-warning.checkbox-inline label{color:#c09853}.has-warning .form-control{border-color:#c09853;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.075);box-shadow:inset 0 1px 1px rgba(0,0,0,0.075)}.has-warning .form-control:focus{border-color:#a47e3c;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.075),0 0 6px #dbc59e;box-shadow:inset 0 1px 1px rgba(0,0,0,0.075),0 0 6px #dbc59e}.has-warning .input-group-addon{color:#c09853;border-color:#c09853;background-color:#fcf8e3}.has-warning .form-control-feedback{color:#c09853}.has-error .help-block,.has-error .control-label,.has-error .radio,.has-error .checkbox,.has-error .radio-inline,.has-error .checkbox-inline,.has-error.radio label,.has-error.checkbox label,.has-error.radio-inline label,.has-error.checkbox-inline label{color:#b94a48}.has-error .form-control{border-color:#b94a48;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.075);box-shadow:inset 0 1px 1px rgba(0,0,0,0.075)}.has-error .form-control:focus{border-color:#953b39;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.075),0 0 6px #d59392;box-shadow:inset 0 1px 1px rgba(0,0,0,0.075),0 0 6px #d59392}.has-error .input-group-addon{color:#b94a48;border-color:#b94a48;background-color:#f2dede}.has-error .form-control-feedback{color:#b94a48}.has-feedback label~.form-control-feedback{top:25px}.has-feedback label.sr-only~.form-control-feedback{top:0}.help-block{display:block;margin-top:5px;margin-bottom:10px;color:#959595}@media (min-width:768px){.form-inline .form-group{display:inline-block;margin-bottom:0;vertical-align:middle}.form-inline .form-control{display:inline-block;width:auto;vertical-align:middle}.form-inline .form-control-static{display:inline-block}.form-inline .input-group{display:inline-table;vertical-align:middle}.form-inline .input-group .input-group-addon,.form-inline .input-group .input-group-btn,.form-inline .input-group .form-control{width:auto}.form-inline .input-group>.form-control{width:100%}.form-inline .control-label{margin-bottom:0;vertical-align:middle}.form-inline .radio,.form-inline .checkbox{display:inline-block;margin-top:0;margin-bottom:0;vertical-align:middle}.form-inline .radio label,.form-inline .checkbox label{padding-left:0}.form-inline .radio input[type="radio"],.form-inline .checkbox input[type="checkbox"]{position:relative;margin-left:0}.form-inline .has-feedback .form-control-feedback{top:0}}.form-horizontal .radio,.form-horizontal .checkbox,.form-horizontal .radio-inline,.form-horizontal .checkbox-inline{margin-top:0;margin-bottom:0;padding-top:9px}.form-horizontal .radio,.form-horizontal .checkbox{min-height:29px}.form-horizontal .form-group{margin-left:-15px;margin-right:-15px}@media (min-width:768px){.form-horizontal .control-label{text-align:right;margin-bottom:0;padding-top:9px}}.form-horizontal .has-feedback .form-control-feedback{right:15px}@media (min-width:768px){.form-horizontal .form-group-lg .control-label{padding-top:15px;font-size:18px}}@media (min-width:768px){.form-horizontal .form-group-sm .control-label{padding-top:6px;font-size:12px}}.btn{display:inline-block;margin-bottom:0;font-weight:normal;text-align:center;vertical-align:middle;-ms-touch-action:manipulation;touch-action:manipulation;cursor:pointer;background-image:none;border:1px solid transparent;white-space:nowrap;padding:8px 12px;font-size:14px;line-height:1.42857143;border-radius:4px;-webkit-user-select:none;-moz-user-select:none;-ms-user-select:none;user-select:none}.btn:focus,.btn:active:focus,.btn.active:focus,.btn.focus,.btn:active.focus,.btn.active.focus{outline:5px auto -webkit-focus-ring-color;outline-offset:-2px}.btn:hover,.btn:focus,.btn.focus{color:#555555;text-decoration:none}.btn:active,.btn.active{outline:0;background-image:none;-webkit-box-shadow:inset 0 3px 5px rgba(0,0,0,0.125);box-shadow:inset 0 3px 5px rgba(0,0,0,0.125)}.btn.disabled,.btn[disabled],fieldset[disabled] .btn{cursor:not-allowed;opacity:0.65;filter:alpha(opacity=65);-webkit-box-shadow:none;box-shadow:none}a.btn.disabled,fieldset[disabled] a.btn{pointer-events:none}.btn-default{color:#555555;background-color:#ffffff;border-color:rgba(0,0,0,0.1)}.btn-default:focus,.btn-default.focus{color:#555555;background-color:#e6e6e6;border-color:rgba(0,0,0,0.1)}.btn-default:hover{color:#555555;background-color:#e6e6e6;border-color:rgba(0,0,0,0.1)}.btn-default:active,.btn-default.active,.open>.dropdown-toggle.btn-default{color:#555555;background-color:#e6e6e6;border-color:rgba(0,0,0,0.1)}.btn-default:active:hover,.btn-default.active:hover,.open>.dropdown-toggle.btn-default:hover,.btn-default:active:focus,.btn-default.active:focus,.open>.dropdown-toggle.btn-default:focus,.btn-default:active.focus,.btn-default.active.focus,.open>.dropdown-toggle.btn-default.focus{color:#555555;background-color:#d4d4d4;border-color:rgba(0,0,0,0.1)}.btn-default:active,.btn-default.active,.open>.dropdown-toggle.btn-default{background-image:none}.btn-default.disabled:hover,.btn-default[disabled]:hover,fieldset[disabled] .btn-default:hover,.btn-default.disabled:focus,.btn-default[disabled]:focus,fieldset[disabled] .btn-default:focus,.btn-default.disabled.focus,.btn-default[disabled].focus,fieldset[disabled] .btn-default.focus{background-color:#ffffff;border-color:rgba(0,0,0,0.1)}.btn-default .badge{color:#ffffff;background-color:#555555}.btn-primary{color:#ffffff;background-color:#2fa4e7;border-color:#2fa4e7}.btn-primary:focus,.btn-primary.focus{color:#ffffff;background-color:#178acc;border-color:#105b87}.btn-primary:hover{color:#ffffff;background-color:#178acc;border-color:#1684c2}.btn-primary:active,.btn-primary.active,.open>.dropdown-toggle.btn-primary{color:#ffffff;background-color:#178acc;border-color:#1684c2}.btn-primary:active:hover,.btn-primary.active:hover,.open>.dropdown-toggle.btn-primary:hover,.btn-primary:active:focus,.btn-primary.active:focus,.open>.dropdown-toggle.btn-primary:focus,.btn-primary:active.focus,.btn-primary.active.focus,.open>.dropdown-toggle.btn-primary.focus{color:#ffffff;background-color:#1474ac;border-color:#105b87}.btn-primary:active,.btn-primary.active,.open>.dropdown-toggle.btn-primary{background-image:none}.btn-primary.disabled:hover,.btn-primary[disabled]:hover,fieldset[disabled] .btn-primary:hover,.btn-primary.disabled:focus,.btn-primary[disabled]:focus,fieldset[disabled] .btn-primary:focus,.btn-primary.disabled.focus,.btn-primary[disabled].focus,fieldset[disabled] .btn-primary.focus{background-color:#2fa4e7;border-color:#2fa4e7}.btn-primary .badge{color:#2fa4e7;background-color:#ffffff}.btn-success{color:#ffffff;background-color:#73a839;border-color:#73a839}.btn-success:focus,.btn-success.focus{color:#ffffff;background-color:#59822c;border-color:#324919}.btn-success:hover{color:#ffffff;background-color:#59822c;border-color:#547a29}.btn-success:active,.btn-success.active,.open>.dropdown-toggle.btn-success{color:#ffffff;background-color:#59822c;border-color:#547a29}.btn-success:active:hover,.btn-success.active:hover,.open>.dropdown-toggle.btn-success:hover,.btn-success:active:focus,.btn-success.active:focus,.open>.dropdown-toggle.btn-success:focus,.btn-success:active.focus,.btn-success.active.focus,.open>.dropdown-toggle.btn-success.focus{color:#ffffff;background-color:#476723;border-color:#324919}.btn-success:active,.btn-success.active,.open>.dropdown-toggle.btn-success{background-image:none}.btn-success.disabled:hover,.btn-success[disabled]:hover,fieldset[disabled] .btn-success:hover,.btn-success.disabled:focus,.btn-success[disabled]:focus,fieldset[disabled] .btn-success:focus,.btn-success.disabled.focus,.btn-success[disabled].focus,fieldset[disabled] .btn-success.focus{background-color:#73a839;border-color:#73a839}.btn-success .badge{color:#73a839;background-color:#ffffff}.btn-info{color:#ffffff;background-color:#033c73;border-color:#033c73}.btn-info:focus,.btn-info.focus{color:#ffffff;background-color:#022241;border-color:#000000}.btn-info:hover{color:#ffffff;background-color:#022241;border-color:#011d37}.btn-info:active,.btn-info.active,.open>.dropdown-toggle.btn-info{color:#ffffff;background-color:#022241;border-color:#011d37}.btn-info:active:hover,.btn-info.active:hover,.open>.dropdown-toggle.btn-info:hover,.btn-info:active:focus,.btn-info.active:focus,.open>.dropdown-toggle.btn-info:focus,.btn-info:active.focus,.btn-info.active.focus,.open>.dropdown-toggle.btn-info.focus{color:#ffffff;background-color:#01101f;border-color:#000000}.btn-info:active,.btn-info.active,.open>.dropdown-toggle.btn-info{background-image:none}.btn-info.disabled:hover,.btn-info[disabled]:hover,fieldset[disabled] .btn-info:hover,.btn-info.disabled:focus,.btn-info[disabled]:focus,fieldset[disabled] .btn-info:focus,.btn-info.disabled.focus,.btn-info[disabled].focus,fieldset[disabled] .btn-info.focus{background-color:#033c73;border-color:#033c73}.btn-info .badge{color:#033c73;background-color:#ffffff}.btn-warning{color:#ffffff;background-color:#dd5600;border-color:#dd5600}.btn-warning:focus,.btn-warning.focus{color:#ffffff;background-color:#aa4200;border-color:#5e2400}.btn-warning:hover{color:#ffffff;background-color:#aa4200;border-color:#a03e00}.btn-warning:active,.btn-warning.active,.open>.dropdown-toggle.btn-warning{color:#ffffff;background-color:#aa4200;border-color:#a03e00}.btn-warning:active:hover,.btn-warning.active:hover,.open>.dropdown-toggle.btn-warning:hover,.btn-warning:active:focus,.btn-warning.active:focus,.open>.dropdown-toggle.btn-warning:focus,.btn-warning:active.focus,.btn-warning.active.focus,.open>.dropdown-toggle.btn-warning.focus{color:#ffffff;background-color:#863400;border-color:#5e2400}.btn-warning:active,.btn-warning.active,.open>.dropdown-toggle.btn-warning{background-image:none}.btn-warning.disabled:hover,.btn-warning[disabled]:hover,fieldset[disabled] .btn-warning:hover,.btn-warning.disabled:focus,.btn-warning[disabled]:focus,fieldset[disabled] .btn-warning:focus,.btn-warning.disabled.focus,.btn-warning[disabled].focus,fieldset[disabled] .btn-warning.focus{background-color:#dd5600;border-color:#dd5600}.btn-warning .badge{color:#dd5600;background-color:#ffffff}.btn-danger{color:#ffffff;background-color:#c71c22;border-color:#c71c22}.btn-danger:focus,.btn-danger.focus{color:#ffffff;background-color:#9a161a;border-color:#570c0f}.btn-danger:hover{color:#ffffff;background-color:#9a161a;border-color:#911419}.btn-danger:active,.btn-danger.active,.open>.dropdown-toggle.btn-danger{color:#ffffff;background-color:#9a161a;border-color:#911419}.btn-danger:active:hover,.btn-danger.active:hover,.open>.dropdown-toggle.btn-danger:hover,.btn-danger:active:focus,.btn-danger.active:focus,.open>.dropdown-toggle.btn-danger:focus,.btn-danger:active.focus,.btn-danger.active.focus,.open>.dropdown-toggle.btn-danger.focus{color:#ffffff;background-color:#7b1115;border-color:#570c0f}.btn-danger:active,.btn-danger.active,.open>.dropdown-toggle.btn-danger{background-image:none}.btn-danger.disabled:hover,.btn-danger[disabled]:hover,fieldset[disabled] .btn-danger:hover,.btn-danger.disabled:focus,.btn-danger[disabled]:focus,fieldset[disabled] .btn-danger:focus,.btn-danger.disabled.focus,.btn-danger[disabled].focus,fieldset[disabled] .btn-danger.focus{background-color:#c71c22;border-color:#c71c22}.btn-danger .badge{color:#c71c22;background-color:#ffffff}.btn-link{color:#2fa4e7;font-weight:normal;border-radius:0}.btn-link,.btn-link:active,.btn-link.active,.btn-link[disabled],fieldset[disabled] .btn-link{background-color:transparent;-webkit-box-shadow:none;box-shadow:none}.btn-link,.btn-link:hover,.btn-link:focus,.btn-link:active{border-color:transparent}.btn-link:hover,.btn-link:focus{color:#157ab5;text-decoration:underline;background-color:transparent}.btn-link[disabled]:hover,fieldset[disabled] .btn-link:hover,.btn-link[disabled]:focus,fieldset[disabled] .btn-link:focus{color:#999999;text-decoration:none}.btn-lg,.btn-group-lg>.btn{padding:14px 16px;font-size:18px;line-height:1.3333333;border-radius:6px}.btn-sm,.btn-group-sm>.btn{padding:5px 10px;font-size:12px;line-height:1.5;border-radius:3px}.btn-xs,.btn-group-xs>.btn{padding:1px 5px;font-size:12px;line-height:1.5;border-radius:3px}.btn-block{display:block;width:100%}.btn-block+.btn-block{margin-top:5px}input[type="submit"].btn-block,input[type="reset"].btn-block,input[type="button"].btn-block{width:100%}.fade{opacity:0;-webkit-transition:opacity 0.15s linear;-o-transition:opacity 0.15s linear;transition:opacity 0.15s linear}.fade.in{opacity:1}.collapse{display:none}.collapse.in{display:block}tr.collapse.in{display:table-row}tbody.collapse.in{display:table-row-group}.collapsing{position:relative;height:0;overflow:hidden;-webkit-transition-property:height, visibility;-o-transition-property:height, visibility;transition-property:height, visibility;-webkit-transition-duration:0.35s;-o-transition-duration:0.35s;transition-duration:0.35s;-webkit-transition-timing-function:ease;-o-transition-timing-function:ease;transition-timing-function:ease}.caret{display:inline-block;width:0;height:0;margin-left:2px;vertical-align:middle;border-top:4px dashed;border-top:4px solid \9;border-right:4px solid transparent;border-left:4px solid transparent}.dropup,.dropdown{position:relative}.dropdown-toggle:focus{outline:0}.dropdown-menu{position:absolute;top:100%;left:0;z-index:1000;display:none;float:left;min-width:160px;padding:5px 0;margin:2px 0 0;list-style:none;font-size:14px;text-align:left;background-color:#ffffff;border:1px solid #cccccc;border:1px solid rgba(0,0,0,0.15);border-radius:4px;-webkit-box-shadow:0 6px 12px rgba(0,0,0,0.175);box-shadow:0 6px 12px rgba(0,0,0,0.175);-webkit-background-clip:padding-box;background-clip:padding-box}.dropdown-menu.pull-right{right:0;left:auto}.dropdown-menu .divider{height:1px;margin:9px 0;overflow:hidden;background-color:#e5e5e5}.dropdown-menu>li>a{display:block;padding:3px 20px;clear:both;font-weight:normal;line-height:1.42857143;color:#333333;white-space:nowrap}.dropdown-menu>li>a:hover,.dropdown-menu>li>a:focus{text-decoration:none;color:#ffffff;background-color:#2fa4e7}.dropdown-menu>.active>a,.dropdown-menu>.active>a:hover,.dropdown-menu>.active>a:focus{color:#ffffff;text-decoration:none;outline:0;background-color:#2fa4e7}.dropdown-menu>.disabled>a,.dropdown-menu>.disabled>a:hover,.dropdown-menu>.disabled>a:focus{color:#999999}.dropdown-menu>.disabled>a:hover,.dropdown-menu>.disabled>a:focus{text-decoration:none;background-color:transparent;background-image:none;filter:progid:DXImageTransform.Microsoft.gradient(enabled=false);cursor:not-allowed}.open>.dropdown-menu{display:block}.open>a{outline:0}.dropdown-menu-right{left:auto;right:0}.dropdown-menu-left{left:0;right:auto}.dropdown-header{display:block;padding:3px 20px;font-size:12px;line-height:1.42857143;color:#999999;white-space:nowrap}.dropdown-backdrop{position:fixed;left:0;right:0;bottom:0;top:0;z-index:990}.pull-right>.dropdown-menu{right:0;left:auto}.dropup .caret,.navbar-fixed-bottom .dropdown .caret{border-top:0;border-bottom:4px dashed;border-bottom:4px solid \9;content:""}.dropup .dropdown-menu,.navbar-fixed-bottom .dropdown .dropdown-menu{top:auto;bottom:100%;margin-bottom:2px}@media (min-width:768px){.navbar-right .dropdown-menu{left:auto;right:0}.navbar-right .dropdown-menu-left{left:0;right:auto}}.btn-group,.btn-group-vertical{position:relative;display:inline-block;vertical-align:middle}.btn-group>.btn,.btn-group-vertical>.btn{position:relative;float:left}.btn-group>.btn:hover,.btn-group-vertical>.btn:hover,.btn-group>.btn:focus,.btn-group-vertical>.btn:focus,.btn-group>.btn:active,.btn-group-vertical>.btn:active,.btn-group>.btn.active,.btn-group-vertical>.btn.active{z-index:2}.btn-group .btn+.btn,.btn-group .btn+.btn-group,.btn-group .btn-group+.btn,.btn-group .btn-group+.btn-group{margin-left:-1px}.btn-toolbar{margin-left:-5px}.btn-toolbar .btn,.btn-toolbar .btn-group,.btn-toolbar .input-group{float:left}.btn-toolbar>.btn,.btn-toolbar>.btn-group,.btn-toolbar>.input-group{margin-left:5px}.btn-group>.btn:not(:first-child):not(:last-child):not(.dropdown-toggle){border-radius:0}.btn-group>.btn:first-child{margin-left:0}.btn-group>.btn:first-child:not(:last-child):not(.dropdown-toggle){border-bottom-right-radius:0;border-top-right-radius:0}.btn-group>.btn:last-child:not(:first-child),.btn-group>.dropdown-toggle:not(:first-child){border-bottom-left-radius:0;border-top-left-radius:0}.btn-group>.btn-group{float:left}.btn-group>.btn-group:not(:first-child):not(:last-child)>.btn{border-radius:0}.btn-group>.btn-group:first-child:not(:last-child)>.btn:last-child,.btn-group>.btn-group:first-child:not(:last-child)>.dropdown-toggle{border-bottom-right-radius:0;border-top-right-radius:0}.btn-group>.btn-group:last-child:not(:first-child)>.btn:first-child{border-bottom-left-radius:0;border-top-left-radius:0}.btn-group .dropdown-toggle:active,.btn-group.open .dropdown-toggle{outline:0}.btn-group>.btn+.dropdown-toggle{padding-left:8px;padding-right:8px}.btn-group>.btn-lg+.dropdown-toggle{padding-left:12px;padding-right:12px}.btn-group.open .dropdown-toggle{-webkit-box-shadow:inset 0 3px 5px rgba(0,0,0,0.125);box-shadow:inset 0 3px 5px rgba(0,0,0,0.125)}.btn-group.open .dropdown-toggle.btn-link{-webkit-box-shadow:none;box-shadow:none}.btn .caret{margin-left:0}.btn-lg .caret{border-width:5px 5px 0;border-bottom-width:0}.dropup .btn-lg .caret{border-width:0 5px 5px}.btn-group-vertical>.btn,.btn-group-vertical>.btn-group,.btn-group-vertical>.btn-group>.btn{display:block;float:none;width:100%;max-width:100%}.btn-group-vertical>.btn-group>.btn{float:none}.btn-group-vertical>.btn+.btn,.btn-group-vertical>.btn+.btn-group,.btn-group-vertical>.btn-group+.btn,.btn-group-vertical>.btn-group+.btn-group{margin-top:-1px;margin-left:0}.btn-group-vertical>.btn:not(:first-child):not(:last-child){border-radius:0}.btn-group-vertical>.btn:first-child:not(:last-child){border-top-right-radius:4px;border-top-left-radius:4px;border-bottom-right-radius:0;border-bottom-left-radius:0}.btn-group-vertical>.btn:last-child:not(:first-child){border-top-right-radius:0;border-top-left-radius:0;border-bottom-right-radius:4px;border-bottom-left-radius:4px}.btn-group-vertical>.btn-group:not(:first-child):not(:last-child)>.btn{border-radius:0}.btn-group-vertical>.btn-group:first-child:not(:last-child)>.btn:last-child,.btn-group-vertical>.btn-group:first-child:not(:last-child)>.dropdown-toggle{border-bottom-right-radius:0;border-bottom-left-radius:0}.btn-group-vertical>.btn-group:last-child:not(:first-child)>.btn:first-child{border-top-right-radius:0;border-top-left-radius:0}.btn-group-justified{display:table;width:100%;table-layout:fixed;border-collapse:separate}.btn-group-justified>.btn,.btn-group-justified>.btn-group{float:none;display:table-cell;width:1%}.btn-group-justified>.btn-group .btn{width:100%}.btn-group-justified>.btn-group .dropdown-menu{left:auto}[data-toggle="buttons"]>.btn input[type="radio"],[data-toggle="buttons"]>.btn-group>.btn input[type="radio"],[data-toggle="buttons"]>.btn input[type="checkbox"],[data-toggle="buttons"]>.btn-group>.btn input[type="checkbox"]{position:absolute;clip:rect(0, 0, 0, 0);pointer-events:none}.input-group{position:relative;display:table;border-collapse:separate}.input-group[class*="col-"]{float:none;padding-left:0;padding-right:0}.input-group .form-control{position:relative;z-index:2;float:left;width:100%;margin-bottom:0}.input-group .form-control:focus{z-index:3}.input-group-lg>.form-control,.input-group-lg>.input-group-addon,.input-group-lg>.input-group-btn>.btn{height:54px;padding:14px 16px;font-size:18px;line-height:1.3333333;border-radius:6px}select.input-group-lg>.form-control,select.input-group-lg>.input-group-addon,select.input-group-lg>.input-group-btn>.btn{height:54px;line-height:54px}textarea.input-group-lg>.form-control,textarea.input-group-lg>.input-group-addon,textarea.input-group-lg>.input-group-btn>.btn,select[multiple].input-group-lg>.form-control,select[multiple].input-group-lg>.input-group-addon,select[multiple].input-group-lg>.input-group-btn>.btn{height:auto}.input-group-sm>.form-control,.input-group-sm>.input-group-addon,.input-group-sm>.input-group-btn>.btn{height:30px;padding:5px 10px;font-size:12px;line-height:1.5;border-radius:3px}select.input-group-sm>.form-control,select.input-group-sm>.input-group-addon,select.input-group-sm>.input-group-btn>.btn{height:30px;line-height:30px}textarea.input-group-sm>.form-control,textarea.input-group-sm>.input-group-addon,textarea.input-group-sm>.input-group-btn>.btn,select[multiple].input-group-sm>.form-control,select[multiple].input-group-sm>.input-group-addon,select[multiple].input-group-sm>.input-group-btn>.btn{height:auto}.input-group-addon,.input-group-btn,.input-group .form-control{display:table-cell}.input-group-addon:not(:first-child):not(:last-child),.input-group-btn:not(:first-child):not(:last-child),.input-group .form-control:not(:first-child):not(:last-child){border-radius:0}.input-group-addon,.input-group-btn{width:1%;white-space:nowrap;vertical-align:middle}.input-group-addon{padding:8px 12px;font-size:14px;font-weight:normal;line-height:1;color:#555555;text-align:center;background-color:#eeeeee;border:1px solid #cccccc;border-radius:4px}.input-group-addon.input-sm{padding:5px 10px;font-size:12px;border-radius:3px}.input-group-addon.input-lg{padding:14px 16px;font-size:18px;border-radius:6px}.input-group-addon input[type="radio"],.input-group-addon input[type="checkbox"]{margin-top:0}.input-group .form-control:first-child,.input-group-addon:first-child,.input-group-btn:first-child>.btn,.input-group-btn:first-child>.btn-group>.btn,.input-group-btn:first-child>.dropdown-toggle,.input-group-btn:last-child>.btn:not(:last-child):not(.dropdown-toggle),.input-group-btn:last-child>.btn-group:not(:last-child)>.btn{border-bottom-right-radius:0;border-top-right-radius:0}.input-group-addon:first-child{border-right:0}.input-group .form-control:last-child,.input-group-addon:last-child,.input-group-btn:last-child>.btn,.input-group-btn:last-child>.btn-group>.btn,.input-group-btn:last-child>.dropdown-toggle,.input-group-btn:first-child>.btn:not(:first-child),.input-group-btn:first-child>.btn-group:not(:first-child)>.btn{border-bottom-left-radius:0;border-top-left-radius:0}.input-group-addon:last-child{border-left:0}.input-group-btn{position:relative;font-size:0;white-space:nowrap}.input-group-btn>.btn{position:relative}.input-group-btn>.btn+.btn{margin-left:-1px}.input-group-btn>.btn:hover,.input-group-btn>.btn:focus,.input-group-btn>.btn:active{z-index:2}.input-group-btn:first-child>.btn,.input-group-btn:first-child>.btn-group{margin-right:-1px}.input-group-btn:last-child>.btn,.input-group-btn:last-child>.btn-group{z-index:2;margin-left:-1px}.nav{margin-bottom:0;padding-left:0;list-style:none}.nav>li{position:relative;display:block}.nav>li>a{position:relative;display:block;padding:10px 15px}.nav>li>a:hover,.nav>li>a:focus{text-decoration:none;background-color:#eeeeee}.nav>li.disabled>a{color:#999999}.nav>li.disabled>a:hover,.nav>li.disabled>a:focus{color:#999999;text-decoration:none;background-color:transparent;cursor:not-allowed}.nav .open>a,.nav .open>a:hover,.nav .open>a:focus{background-color:#eeeeee;border-color:#2fa4e7}.nav .nav-divider{height:1px;margin:9px 0;overflow:hidden;background-color:#e5e5e5}.nav>li>a>img{max-width:none}.nav-tabs{border-bottom:1px solid #dddddd}.nav-tabs>li{float:left;margin-bottom:-1px}.nav-tabs>li>a{margin-right:2px;line-height:1.42857143;border:1px solid transparent;border-radius:4px 4px 0 0}.nav-tabs>li>a:hover{border-color:#eeeeee #eeeeee #dddddd}.nav-tabs>li.active>a,.nav-tabs>li.active>a:hover,.nav-tabs>li.active>a:focus{color:#555555;background-color:#ffffff;border:1px solid #dddddd;border-bottom-color:transparent;cursor:default}.nav-tabs.nav-justified{width:100%;border-bottom:0}.nav-tabs.nav-justified>li{float:none}.nav-tabs.nav-justified>li>a{text-align:center;margin-bottom:5px}.nav-tabs.nav-justified>.dropdown .dropdown-menu{top:auto;left:auto}@media (min-width:768px){.nav-tabs.nav-justified>li{display:table-cell;width:1%}.nav-tabs.nav-justified>li>a{margin-bottom:0}}.nav-tabs.nav-justified>li>a{margin-right:0;border-radius:4px}.nav-tabs.nav-justified>.active>a,.nav-tabs.nav-justified>.active>a:hover,.nav-tabs.nav-justified>.active>a:focus{border:1px solid #dddddd}@media (min-width:768px){.nav-tabs.nav-justified>li>a{border-bottom:1px solid #dddddd;border-radius:4px 4px 0 0}.nav-tabs.nav-justified>.active>a,.nav-tabs.nav-justified>.active>a:hover,.nav-tabs.nav-justified>.active>a:focus{border-bottom-color:#ffffff}}.nav-pills>li{float:left}.nav-pills>li>a{border-radius:4px}.nav-pills>li+li{margin-left:2px}.nav-pills>li.active>a,.nav-pills>li.active>a:hover,.nav-pills>li.active>a:focus{color:#ffffff;background-color:#2fa4e7}.nav-stacked>li{float:none}.nav-stacked>li+li{margin-top:2px;margin-left:0}.nav-justified{width:100%}.nav-justified>li{float:none}.nav-justified>li>a{text-align:center;margin-bottom:5px}.nav-justified>.dropdown .dropdown-menu{top:auto;left:auto}@media (min-width:768px){.nav-justified>li{display:table-cell;width:1%}.nav-justified>li>a{margin-bottom:0}}.nav-tabs-justified{border-bottom:0}.nav-tabs-justified>li>a{margin-right:0;border-radius:4px}.nav-tabs-justified>.active>a,.nav-tabs-justified>.active>a:hover,.nav-tabs-justified>.active>a:focus{border:1px solid #dddddd}@media (min-width:768px){.nav-tabs-justified>li>a{border-bottom:1px solid #dddddd;border-radius:4px 4px 0 0}.nav-tabs-justified>.active>a,.nav-tabs-justified>.active>a:hover,.nav-tabs-justified>.active>a:focus{border-bottom-color:#ffffff}}.tab-content>.tab-pane{display:none}.tab-content>.active{display:block}.nav-tabs .dropdown-menu{margin-top:-1px;border-top-right-radius:0;border-top-left-radius:0}.navbar{position:relative;min-height:50px;margin-bottom:20px;border:1px solid transparent}@media (min-width:768px){.navbar{border-radius:4px}}@media (min-width:768px){.navbar-header{float:left}}.navbar-collapse{overflow-x:visible;padding-right:15px;padding-left:15px;border-top:1px solid transparent;-webkit-box-shadow:inset 0 1px 0 rgba(255,255,255,0.1);box-shadow:inset 0 1px 0 rgba(255,255,255,0.1);-webkit-overflow-scrolling:touch}.navbar-collapse.in{overflow-y:auto}@media (min-width:768px){.navbar-collapse{width:auto;border-top:0;-webkit-box-shadow:none;box-shadow:none}.navbar-collapse.collapse{display:block !important;height:auto !important;padding-bottom:0;overflow:visible !important}.navbar-collapse.in{overflow-y:visible}.navbar-fixed-top .navbar-collapse,.navbar-static-top .navbar-collapse,.navbar-fixed-bottom .navbar-collapse{padding-left:0;padding-right:0}}.navbar-fixed-top .navbar-collapse,.navbar-fixed-bottom .navbar-collapse{max-height:340px}@media (max-device-width:480px) and (orientation:landscape){.navbar-fixed-top .navbar-collapse,.navbar-fixed-bottom .navbar-collapse{max-height:200px}}.container>.navbar-header,.container-fluid>.navbar-header,.container>.navbar-collapse,.container-fluid>.navbar-collapse{margin-right:-15px;margin-left:-15px}@media (min-width:768px){.container>.navbar-header,.container-fluid>.navbar-header,.container>.navbar-collapse,.container-fluid>.navbar-collapse{margin-right:0;margin-left:0}}.navbar-static-top{z-index:1000;border-width:0 0 1px}@media (min-width:768px){.navbar-static-top{border-radius:0}}.navbar-fixed-top,.navbar-fixed-bottom{position:fixed;right:0;left:0;z-index:1030}@media (min-width:768px){.navbar-fixed-top,.navbar-fixed-bottom{border-radius:0}}.navbar-fixed-top{top:0;border-width:0 0 1px}.navbar-fixed-bottom{bottom:0;margin-bottom:0;border-width:1px 0 0}.navbar-brand{float:left;padding:15px 15px;font-size:18px;line-height:20px;height:50px}.navbar-brand:hover,.navbar-brand:focus{text-decoration:none}.navbar-brand>img{display:block}@media (min-width:768px){.navbar>.container .navbar-brand,.navbar>.container-fluid .navbar-brand{margin-left:-15px}}.navbar-toggle{position:relative;float:right;margin-right:15px;padding:9px 10px;margin-top:8px;margin-bottom:8px;background-color:transparent;background-image:none;border:1px solid transparent;border-radius:4px}.navbar-toggle:focus{outline:0}.navbar-toggle .icon-bar{display:block;width:22px;height:2px;border-radius:1px}.navbar-toggle .icon-bar+.icon-bar{margin-top:4px}@media (min-width:768px){.navbar-toggle{display:none}}.navbar-nav{margin:7.5px -15px}.navbar-nav>li>a{padding-top:10px;padding-bottom:10px;line-height:20px}@media (max-width:767px){.navbar-nav .open .dropdown-menu{position:static;float:none;width:auto;margin-top:0;background-color:transparent;border:0;-webkit-box-shadow:none;box-shadow:none}.navbar-nav .open .dropdown-menu>li>a,.navbar-nav .open .dropdown-menu .dropdown-header{padding:5px 15px 5px 25px}.navbar-nav .open .dropdown-menu>li>a{line-height:20px}.navbar-nav .open .dropdown-menu>li>a:hover,.navbar-nav .open .dropdown-menu>li>a:focus{background-image:none}}@media (min-width:768px){.navbar-nav{float:left;margin:0}.navbar-nav>li{float:left}.navbar-nav>li>a{padding-top:15px;padding-bottom:15px}}.navbar-form{margin-left:-15px;margin-right:-15px;padding:10px 15px;border-top:1px solid transparent;border-bottom:1px solid transparent;-webkit-box-shadow:inset 0 1px 0 rgba(255,255,255,0.1),0 1px 0 rgba(255,255,255,0.1);box-shadow:inset 0 1px 0 rgba(255,255,255,0.1),0 1px 0 rgba(255,255,255,0.1);margin-top:6px;margin-bottom:6px}@media (min-width:768px){.navbar-form .form-group{display:inline-block;margin-bottom:0;vertical-align:middle}.navbar-form .form-control{display:inline-block;width:auto;vertical-align:middle}.navbar-form .form-control-static{display:inline-block}.navbar-form .input-group{display:inline-table;vertical-align:middle}.navbar-form .input-group .input-group-addon,.navbar-form .input-group .input-group-btn,.navbar-form .input-group .form-control{width:auto}.navbar-form .input-group>.form-control{width:100%}.navbar-form .control-label{margin-bottom:0;vertical-align:middle}.navbar-form .radio,.navbar-form .checkbox{display:inline-block;margin-top:0;margin-bottom:0;vertical-align:middle}.navbar-form .radio label,.navbar-form .checkbox label{padding-left:0}.navbar-form .radio input[type="radio"],.navbar-form .checkbox input[type="checkbox"]{position:relative;margin-left:0}.navbar-form .has-feedback .form-control-feedback{top:0}}@media (max-width:767px){.navbar-form .form-group{margin-bottom:5px}.navbar-form .form-group:last-child{margin-bottom:0}}@media (min-width:768px){.navbar-form{width:auto;border:0;margin-left:0;margin-right:0;padding-top:0;padding-bottom:0;-webkit-box-shadow:none;box-shadow:none}}.navbar-nav>li>.dropdown-menu{margin-top:0;border-top-right-radius:0;border-top-left-radius:0}.navbar-fixed-bottom .navbar-nav>li>.dropdown-menu{margin-bottom:0;border-top-right-radius:4px;border-top-left-radius:4px;border-bottom-right-radius:0;border-bottom-left-radius:0}.navbar-btn{margin-top:6px;margin-bottom:6px}.navbar-btn.btn-sm{margin-top:10px;margin-bottom:10px}.navbar-btn.btn-xs{margin-top:14px;margin-bottom:14px}.navbar-text{margin-top:15px;margin-bottom:15px}@media (min-width:768px){.navbar-text{float:left;margin-left:15px;margin-right:15px}}@media (min-width:768px){.navbar-left{float:left !important}.navbar-right{float:right !important;margin-right:-15px}.navbar-right~.navbar-right{margin-right:0}}.navbar-default{background-color:#2fa4e7;border-color:#1995dc}.navbar-default .navbar-brand{color:#ffffff}.navbar-default .navbar-brand:hover,.navbar-default .navbar-brand:focus{color:#ffffff;background-color:none}.navbar-default .navbar-text{color:#dddddd}.navbar-default .navbar-nav>li>a{color:#ffffff}.navbar-default .navbar-nav>li>a:hover,.navbar-default .navbar-nav>li>a:focus{color:#ffffff;background-color:#178acc}.navbar-default .navbar-nav>.active>a,.navbar-default .navbar-nav>.active>a:hover,.navbar-default .navbar-nav>.active>a:focus{color:#ffffff;background-color:#178acc}.navbar-default .navbar-nav>.disabled>a,.navbar-default .navbar-nav>.disabled>a:hover,.navbar-default .navbar-nav>.disabled>a:focus{color:#dddddd;background-color:transparent}.navbar-default .navbar-toggle{border-color:#178acc}.navbar-default .navbar-toggle:hover,.navbar-default .navbar-toggle:focus{background-color:#178acc}.navbar-default .navbar-toggle .icon-bar{background-color:#ffffff}.navbar-default .navbar-collapse,.navbar-default .navbar-form{border-color:#1995dc}.navbar-default .navbar-nav>.open>a,.navbar-default .navbar-nav>.open>a:hover,.navbar-default .navbar-nav>.open>a:focus{background-color:#178acc;color:#ffffff}@media (max-width:767px){.navbar-default .navbar-nav .open .dropdown-menu>li>a{color:#ffffff}.navbar-default .navbar-nav .open .dropdown-menu>li>a:hover,.navbar-default .navbar-nav .open .dropdown-menu>li>a:focus{color:#ffffff;background-color:#178acc}.navbar-default .navbar-nav .open .dropdown-menu>.active>a,.navbar-default .navbar-nav .open .dropdown-menu>.active>a:hover,.navbar-default .navbar-nav .open .dropdown-menu>.active>a:focus{color:#ffffff;background-color:#178acc}.navbar-default .navbar-nav .open .dropdown-menu>.disabled>a,.navbar-default .navbar-nav .open .dropdown-menu>.disabled>a:hover,.navbar-default .navbar-nav .open .dropdown-menu>.disabled>a:focus{color:#dddddd;background-color:transparent}}.navbar-default .navbar-link{color:#ffffff}.navbar-default .navbar-link:hover{color:#ffffff}.navbar-default .btn-link{color:#ffffff}.navbar-default .btn-link:hover,.navbar-default .btn-link:focus{color:#ffffff}.navbar-default .btn-link[disabled]:hover,fieldset[disabled] .navbar-default .btn-link:hover,.navbar-default .btn-link[disabled]:focus,fieldset[disabled] .navbar-default .btn-link:focus{color:#dddddd}.navbar-inverse{background-color:#033c73;border-color:#022f5a}.navbar-inverse .navbar-brand{color:#ffffff}.navbar-inverse .navbar-brand:hover,.navbar-inverse .navbar-brand:focus{color:#ffffff;background-color:none}.navbar-inverse .navbar-text{color:#ffffff}.navbar-inverse .navbar-nav>li>a{color:#ffffff}.navbar-inverse .navbar-nav>li>a:hover,.navbar-inverse .navbar-nav>li>a:focus{color:#ffffff;background-color:#022f5a}.navbar-inverse .navbar-nav>.active>a,.navbar-inverse .navbar-nav>.active>a:hover,.navbar-inverse .navbar-nav>.active>a:focus{color:#ffffff;background-color:#022f5a}.navbar-inverse .navbar-nav>.disabled>a,.navbar-inverse .navbar-nav>.disabled>a:hover,.navbar-inverse .navbar-nav>.disabled>a:focus{color:#cccccc;background-color:transparent}.navbar-inverse .navbar-toggle{border-color:#022f5a}.navbar-inverse .navbar-toggle:hover,.navbar-inverse .navbar-toggle:focus{background-color:#022f5a}.navbar-inverse .navbar-toggle .icon-bar{background-color:#ffffff}.navbar-inverse .navbar-collapse,.navbar-inverse .navbar-form{border-color:#022a50}.navbar-inverse .navbar-nav>.open>a,.navbar-inverse .navbar-nav>.open>a:hover,.navbar-inverse .navbar-nav>.open>a:focus{background-color:#022f5a;color:#ffffff}@media (max-width:767px){.navbar-inverse .navbar-nav .open .dropdown-menu>.dropdown-header{border-color:#022f5a}.navbar-inverse .navbar-nav .open .dropdown-menu .divider{background-color:#022f5a}.navbar-inverse .navbar-nav .open .dropdown-menu>li>a{color:#ffffff}.navbar-inverse .navbar-nav .open .dropdown-menu>li>a:hover,.navbar-inverse .navbar-nav .open .dropdown-menu>li>a:focus{color:#ffffff;background-color:#022f5a}.navbar-inverse .navbar-nav .open .dropdown-menu>.active>a,.navbar-inverse .navbar-nav .open .dropdown-menu>.active>a:hover,.navbar-inverse .navbar-nav .open .dropdown-menu>.active>a:focus{color:#ffffff;background-color:#022f5a}.navbar-inverse .navbar-nav .open .dropdown-menu>.disabled>a,.navbar-inverse .navbar-nav .open .dropdown-menu>.disabled>a:hover,.navbar-inverse .navbar-nav .open .dropdown-menu>.disabled>a:focus{color:#cccccc;background-color:transparent}}.navbar-inverse .navbar-link{color:#ffffff}.navbar-inverse .navbar-link:hover{color:#ffffff}.navbar-inverse .btn-link{color:#ffffff}.navbar-inverse .btn-link:hover,.navbar-inverse .btn-link:focus{color:#ffffff}.navbar-inverse .btn-link[disabled]:hover,fieldset[disabled] .navbar-inverse .btn-link:hover,.navbar-inverse .btn-link[disabled]:focus,fieldset[disabled] .navbar-inverse .btn-link:focus{color:#cccccc}.breadcrumb{padding:8px 15px;margin-bottom:20px;list-style:none;background-color:#f5f5f5;border-radius:4px}.breadcrumb>li{display:inline-block}.breadcrumb>li+li:before{content:"/\00a0";padding:0 5px;color:#cccccc}.breadcrumb>.active{color:#999999}.pagination{display:inline-block;padding-left:0;margin:20px 0;border-radius:4px}.pagination>li{display:inline}.pagination>li>a,.pagination>li>span{position:relative;float:left;padding:8px 12px;line-height:1.42857143;text-decoration:none;color:#2fa4e7;background-color:#ffffff;border:1px solid #dddddd;margin-left:-1px}.pagination>li:first-child>a,.pagination>li:first-child>span{margin-left:0;border-bottom-left-radius:4px;border-top-left-radius:4px}.pagination>li:last-child>a,.pagination>li:last-child>span{border-bottom-right-radius:4px;border-top-right-radius:4px}.pagination>li>a:hover,.pagination>li>span:hover,.pagination>li>a:focus,.pagination>li>span:focus{z-index:2;color:#157ab5;background-color:#eeeeee;border-color:#dddddd}.pagination>.active>a,.pagination>.active>span,.pagination>.active>a:hover,.pagination>.active>span:hover,.pagination>.active>a:focus,.pagination>.active>span:focus{z-index:3;color:#999999;background-color:#f5f5f5;border-color:#dddddd;cursor:default}.pagination>.disabled>span,.pagination>.disabled>span:hover,.pagination>.disabled>span:focus,.pagination>.disabled>a,.pagination>.disabled>a:hover,.pagination>.disabled>a:focus{color:#999999;background-color:#ffffff;border-color:#dddddd;cursor:not-allowed}.pagination-lg>li>a,.pagination-lg>li>span{padding:14px 16px;font-size:18px;line-height:1.3333333}.pagination-lg>li:first-child>a,.pagination-lg>li:first-child>span{border-bottom-left-radius:6px;border-top-left-radius:6px}.pagination-lg>li:last-child>a,.pagination-lg>li:last-child>span{border-bottom-right-radius:6px;border-top-right-radius:6px}.pagination-sm>li>a,.pagination-sm>li>span{padding:5px 10px;font-size:12px;line-height:1.5}.pagination-sm>li:first-child>a,.pagination-sm>li:first-child>span{border-bottom-left-radius:3px;border-top-left-radius:3px}.pagination-sm>li:last-child>a,.pagination-sm>li:last-child>span{border-bottom-right-radius:3px;border-top-right-radius:3px}.pager{padding-left:0;margin:20px 0;list-style:none;text-align:center}.pager li{display:inline}.pager li>a,.pager li>span{display:inline-block;padding:5px 14px;background-color:#ffffff;border:1px solid #dddddd;border-radius:15px}.pager li>a:hover,.pager li>a:focus{text-decoration:none;background-color:#eeeeee}.pager .next>a,.pager .next>span{float:right}.pager .previous>a,.pager .previous>span{float:left}.pager .disabled>a,.pager .disabled>a:hover,.pager .disabled>a:focus,.pager .disabled>span{color:#999999;background-color:#ffffff;cursor:not-allowed}.label{display:inline;padding:.2em .6em .3em;font-size:75%;font-weight:bold;line-height:1;color:#ffffff;text-align:center;white-space:nowrap;vertical-align:baseline;border-radius:.25em}a.label:hover,a.label:focus{color:#ffffff;text-decoration:none;cursor:pointer}.label:empty{display:none}.btn .label{position:relative;top:-1px}.label-default{background-color:#999999}.label-default[href]:hover,.label-default[href]:focus{background-color:#808080}.label-primary{background-color:#2fa4e7}.label-primary[href]:hover,.label-primary[href]:focus{background-color:#178acc}.label-success{background-color:#73a839}.label-success[href]:hover,.label-success[href]:focus{background-color:#59822c}.label-info{background-color:#033c73}.label-info[href]:hover,.label-info[href]:focus{background-color:#022241}.label-warning{background-color:#dd5600}.label-warning[href]:hover,.label-warning[href]:focus{background-color:#aa4200}.label-danger{background-color:#c71c22}.label-danger[href]:hover,.label-danger[href]:focus{background-color:#9a161a}.badge{display:inline-block;min-width:10px;padding:3px 7px;font-size:12px;font-weight:bold;color:#ffffff;line-height:1;vertical-align:middle;white-space:nowrap;text-align:center;background-color:#2fa4e7;border-radius:10px}.badge:empty{display:none}.btn .badge{position:relative;top:-1px}.btn-xs .badge,.btn-group-xs>.btn .badge{top:0;padding:1px 5px}a.badge:hover,a.badge:focus{color:#ffffff;text-decoration:none;cursor:pointer}.list-group-item.active>.badge,.nav-pills>.active>a>.badge{color:#2fa4e7;background-color:#ffffff}.list-group-item>.badge{float:right}.list-group-item>.badge+.badge{margin-right:5px}.nav-pills>li>a>.badge{margin-left:3px}.jumbotron{padding-top:30px;padding-bottom:30px;margin-bottom:30px;color:inherit;background-color:#eeeeee}.jumbotron h1,.jumbotron .h1{color:inherit}.jumbotron p{margin-bottom:15px;font-size:21px;font-weight:200}.jumbotron>hr{border-top-color:#d5d5d5}.container .jumbotron,.container-fluid .jumbotron{border-radius:6px;padding-left:15px;padding-right:15px}.jumbotron .container{max-width:100%}@media screen and (min-width:768px){.jumbotron{padding-top:48px;padding-bottom:48px}.container .jumbotron,.container-fluid .jumbotron{padding-left:60px;padding-right:60px}.jumbotron h1,.jumbotron .h1{font-size:63px}}.thumbnail{display:block;padding:4px;margin-bottom:20px;line-height:1.42857143;background-color:#ffffff;border:1px solid #dddddd;border-radius:4px;-webkit-transition:border .2s ease-in-out;-o-transition:border .2s ease-in-out;transition:border .2s ease-in-out}.thumbnail>img,.thumbnail a>img{margin-left:auto;margin-right:auto}a.thumbnail:hover,a.thumbnail:focus,a.thumbnail.active{border-color:#2fa4e7}.thumbnail .caption{padding:9px;color:#555555}.alert{padding:15px;margin-bottom:20px;border:1px solid transparent;border-radius:4px}.alert h4{margin-top:0;color:inherit}.alert .alert-link{font-weight:bold}.alert>p,.alert>ul{margin-bottom:0}.alert>p+p{margin-top:5px}.alert-dismissable,.alert-dismissible{padding-right:35px}.alert-dismissable .close,.alert-dismissible .close{position:relative;top:-2px;right:-21px;color:inherit}.alert-success{background-color:#dff0d8;border-color:#d6e9c6;color:#468847}.alert-success hr{border-top-color:#c9e2b3}.alert-success .alert-link{color:#356635}.alert-info{background-color:#d9edf7;border-color:#bce8f1;color:#3a87ad}.alert-info hr{border-top-color:#a6e1ec}.alert-info .alert-link{color:#2d6987}.alert-warning{background-color:#fcf8e3;border-color:#fbeed5;color:#c09853}.alert-warning hr{border-top-color:#f8e5be}.alert-warning .alert-link{color:#a47e3c}.alert-danger{background-color:#f2dede;border-color:#eed3d7;color:#b94a48}.alert-danger hr{border-top-color:#e6c1c7}.alert-danger .alert-link{color:#953b39}@-webkit-keyframes progress-bar-stripes{from{background-position:40px 0}to{background-position:0 0}}@-o-keyframes progress-bar-stripes{from{background-position:40px 0}to{background-position:0 0}}@keyframes progress-bar-stripes{from{background-position:40px 0}to{background-position:0 0}}.progress{overflow:hidden;height:20px;margin-bottom:20px;background-color:#f5f5f5;border-radius:4px;-webkit-box-shadow:inset 0 1px 2px rgba(0,0,0,0.1);box-shadow:inset 0 1px 2px rgba(0,0,0,0.1)}.progress-bar{float:left;width:0%;height:100%;font-size:12px;line-height:20px;color:#ffffff;text-align:center;background-color:#2fa4e7;-webkit-box-shadow:inset 0 -1px 0 rgba(0,0,0,0.15);box-shadow:inset 0 -1px 0 rgba(0,0,0,0.15);-webkit-transition:width 0.6s ease;-o-transition:width 0.6s ease;transition:width 0.6s ease}.progress-striped .progress-bar,.progress-bar-striped{background-image:-webkit-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:-o-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);-webkit-background-size:40px 40px;background-size:40px 40px}.progress.active .progress-bar,.progress-bar.active{-webkit-animation:progress-bar-stripes 2s linear infinite;-o-animation:progress-bar-stripes 2s linear infinite;animation:progress-bar-stripes 2s linear infinite}.progress-bar-success{background-color:#73a839}.progress-striped .progress-bar-success{background-image:-webkit-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:-o-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent)}.progress-bar-info{background-color:#033c73}.progress-striped .progress-bar-info{background-image:-webkit-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:-o-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent)}.progress-bar-warning{background-color:#dd5600}.progress-striped .progress-bar-warning{background-image:-webkit-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:-o-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent)}.progress-bar-danger{background-color:#c71c22}.progress-striped .progress-bar-danger{background-image:-webkit-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:-o-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent)}.media{margin-top:15px}.media:first-child{margin-top:0}.media,.media-body{zoom:1;overflow:hidden}.media-body{width:10000px}.media-object{display:block}.media-object.img-thumbnail{max-width:none}.media-right,.media>.pull-right{padding-left:10px}.media-left,.media>.pull-left{padding-right:10px}.media-left,.media-right,.media-body{display:table-cell;vertical-align:top}.media-middle{vertical-align:middle}.media-bottom{vertical-align:bottom}.media-heading{margin-top:0;margin-bottom:5px}.media-list{padding-left:0;list-style:none}.list-group{margin-bottom:20px;padding-left:0}.list-group-item{position:relative;display:block;padding:10px 15px;margin-bottom:-1px;background-color:#ffffff;border:1px solid #dddddd}.list-group-item:first-child{border-top-right-radius:4px;border-top-left-radius:4px}.list-group-item:last-child{margin-bottom:0;border-bottom-right-radius:4px;border-bottom-left-radius:4px}a.list-group-item,button.list-group-item{color:#555555}a.list-group-item .list-group-item-heading,button.list-group-item .list-group-item-heading{color:#333333}a.list-group-item:hover,button.list-group-item:hover,a.list-group-item:focus,button.list-group-item:focus{text-decoration:none;color:#555555;background-color:#f5f5f5}button.list-group-item{width:100%;text-align:left}.list-group-item.disabled,.list-group-item.disabled:hover,.list-group-item.disabled:focus{background-color:#eeeeee;color:#999999;cursor:not-allowed}.list-group-item.disabled .list-group-item-heading,.list-group-item.disabled:hover .list-group-item-heading,.list-group-item.disabled:focus .list-group-item-heading{color:inherit}.list-group-item.disabled .list-group-item-text,.list-group-item.disabled:hover .list-group-item-text,.list-group-item.disabled:focus .list-group-item-text{color:#999999}.list-group-item.active,.list-group-item.active:hover,.list-group-item.active:focus{z-index:2;color:#ffffff;background-color:#2fa4e7;border-color:#2fa4e7}.list-group-item.active .list-group-item-heading,.list-group-item.active:hover .list-group-item-heading,.list-group-item.active:focus .list-group-item-heading,.list-group-item.active .list-group-item-heading>small,.list-group-item.active:hover .list-group-item-heading>small,.list-group-item.active:focus .list-group-item-heading>small,.list-group-item.active .list-group-item-heading>.small,.list-group-item.active:hover .list-group-item-heading>.small,.list-group-item.active:focus .list-group-item-heading>.small{color:inherit}.list-group-item.active .list-group-item-text,.list-group-item.active:hover .list-group-item-text,.list-group-item.active:focus .list-group-item-text{color:#e6f4fc}.list-group-item-success{color:#468847;background-color:#dff0d8}a.list-group-item-success,button.list-group-item-success{color:#468847}a.list-group-item-success .list-group-item-heading,button.list-group-item-success .list-group-item-heading{color:inherit}a.list-group-item-success:hover,button.list-group-item-success:hover,a.list-group-item-success:focus,button.list-group-item-success:focus{color:#468847;background-color:#d0e9c6}a.list-group-item-success.active,button.list-group-item-success.active,a.list-group-item-success.active:hover,button.list-group-item-success.active:hover,a.list-group-item-success.active:focus,button.list-group-item-success.active:focus{color:#fff;background-color:#468847;border-color:#468847}.list-group-item-info{color:#3a87ad;background-color:#d9edf7}a.list-group-item-info,button.list-group-item-info{color:#3a87ad}a.list-group-item-info .list-group-item-heading,button.list-group-item-info .list-group-item-heading{color:inherit}a.list-group-item-info:hover,button.list-group-item-info:hover,a.list-group-item-info:focus,button.list-group-item-info:focus{color:#3a87ad;background-color:#c4e3f3}a.list-group-item-info.active,button.list-group-item-info.active,a.list-group-item-info.active:hover,button.list-group-item-info.active:hover,a.list-group-item-info.active:focus,button.list-group-item-info.active:focus{color:#fff;background-color:#3a87ad;border-color:#3a87ad}.list-group-item-warning{color:#c09853;background-color:#fcf8e3}a.list-group-item-warning,button.list-group-item-warning{color:#c09853}a.list-group-item-warning .list-group-item-heading,button.list-group-item-warning .list-group-item-heading{color:inherit}a.list-group-item-warning:hover,button.list-group-item-warning:hover,a.list-group-item-warning:focus,button.list-group-item-warning:focus{color:#c09853;background-color:#faf2cc}a.list-group-item-warning.active,button.list-group-item-warning.active,a.list-group-item-warning.active:hover,button.list-group-item-warning.active:hover,a.list-group-item-warning.active:focus,button.list-group-item-warning.active:focus{color:#fff;background-color:#c09853;border-color:#c09853}.list-group-item-danger{color:#b94a48;background-color:#f2dede}a.list-group-item-danger,button.list-group-item-danger{color:#b94a48}a.list-group-item-danger .list-group-item-heading,button.list-group-item-danger .list-group-item-heading{color:inherit}a.list-group-item-danger:hover,button.list-group-item-danger:hover,a.list-group-item-danger:focus,button.list-group-item-danger:focus{color:#b94a48;background-color:#ebcccc}a.list-group-item-danger.active,button.list-group-item-danger.active,a.list-group-item-danger.active:hover,button.list-group-item-danger.active:hover,a.list-group-item-danger.active:focus,button.list-group-item-danger.active:focus{color:#fff;background-color:#b94a48;border-color:#b94a48}.list-group-item-heading{margin-top:0;margin-bottom:5px}.list-group-item-text{margin-bottom:0;line-height:1.3}.panel{margin-bottom:20px;background-color:#ffffff;border:1px solid transparent;border-radius:4px;-webkit-box-shadow:0 1px 1px rgba(0,0,0,0.05);box-shadow:0 1px 1px rgba(0,0,0,0.05)}.panel-body{padding:15px}.panel-heading{padding:10px 15px;border-bottom:1px solid transparent;border-top-right-radius:3px;border-top-left-radius:3px}.panel-heading>.dropdown .dropdown-toggle{color:inherit}.panel-title{margin-top:0;margin-bottom:0;font-size:16px;color:inherit}.panel-title>a,.panel-title>small,.panel-title>.small,.panel-title>small>a,.panel-title>.small>a{color:inherit}.panel-footer{padding:10px 15px;background-color:#f5f5f5;border-top:1px solid #dddddd;border-bottom-right-radius:3px;border-bottom-left-radius:3px}.panel>.list-group,.panel>.panel-collapse>.list-group{margin-bottom:0}.panel>.list-group .list-group-item,.panel>.panel-collapse>.list-group .list-group-item{border-width:1px 0;border-radius:0}.panel>.list-group:first-child .list-group-item:first-child,.panel>.panel-collapse>.list-group:first-child .list-group-item:first-child{border-top:0;border-top-right-radius:3px;border-top-left-radius:3px}.panel>.list-group:last-child .list-group-item:last-child,.panel>.panel-collapse>.list-group:last-child .list-group-item:last-child{border-bottom:0;border-bottom-right-radius:3px;border-bottom-left-radius:3px}.panel>.panel-heading+.panel-collapse>.list-group .list-group-item:first-child{border-top-right-radius:0;border-top-left-radius:0}.panel-heading+.list-group .list-group-item:first-child{border-top-width:0}.list-group+.panel-footer{border-top-width:0}.panel>.table,.panel>.table-responsive>.table,.panel>.panel-collapse>.table{margin-bottom:0}.panel>.table caption,.panel>.table-responsive>.table caption,.panel>.panel-collapse>.table caption{padding-left:15px;padding-right:15px}.panel>.table:first-child,.panel>.table-responsive:first-child>.table:first-child{border-top-right-radius:3px;border-top-left-radius:3px}.panel>.table:first-child>thead:first-child>tr:first-child,.panel>.table-responsive:first-child>.table:first-child>thead:first-child>tr:first-child,.panel>.table:first-child>tbody:first-child>tr:first-child,.panel>.table-responsive:first-child>.table:first-child>tbody:first-child>tr:first-child{border-top-left-radius:3px;border-top-right-radius:3px}.panel>.table:first-child>thead:first-child>tr:first-child td:first-child,.panel>.table-responsive:first-child>.table:first-child>thead:first-child>tr:first-child td:first-child,.panel>.table:first-child>tbody:first-child>tr:first-child td:first-child,.panel>.table-responsive:first-child>.table:first-child>tbody:first-child>tr:first-child td:first-child,.panel>.table:first-child>thead:first-child>tr:first-child th:first-child,.panel>.table-responsive:first-child>.table:first-child>thead:first-child>tr:first-child th:first-child,.panel>.table:first-child>tbody:first-child>tr:first-child th:first-child,.panel>.table-responsive:first-child>.table:first-child>tbody:first-child>tr:first-child th:first-child{border-top-left-radius:3px}.panel>.table:first-child>thead:first-child>tr:first-child td:last-child,.panel>.table-responsive:first-child>.table:first-child>thead:first-child>tr:first-child td:last-child,.panel>.table:first-child>tbody:first-child>tr:first-child td:last-child,.panel>.table-responsive:first-child>.table:first-child>tbody:first-child>tr:first-child td:last-child,.panel>.table:first-child>thead:first-child>tr:first-child th:last-child,.panel>.table-responsive:first-child>.table:first-child>thead:first-child>tr:first-child th:last-child,.panel>.table:first-child>tbody:first-child>tr:first-child th:last-child,.panel>.table-responsive:first-child>.table:first-child>tbody:first-child>tr:first-child th:last-child{border-top-right-radius:3px}.panel>.table:last-child,.panel>.table-responsive:last-child>.table:last-child{border-bottom-right-radius:3px;border-bottom-left-radius:3px}.panel>.table:last-child>tbody:last-child>tr:last-child,.panel>.table-responsive:last-child>.table:last-child>tbody:last-child>tr:last-child,.panel>.table:last-child>tfoot:last-child>tr:last-child,.panel>.table-responsive:last-child>.table:last-child>tfoot:last-child>tr:last-child{border-bottom-left-radius:3px;border-bottom-right-radius:3px}.panel>.table:last-child>tbody:last-child>tr:last-child td:first-child,.panel>.table-responsive:last-child>.table:last-child>tbody:last-child>tr:last-child td:first-child,.panel>.table:last-child>tfoot:last-child>tr:last-child td:first-child,.panel>.table-responsive:last-child>.table:last-child>tfoot:last-child>tr:last-child td:first-child,.panel>.table:last-child>tbody:last-child>tr:last-child th:first-child,.panel>.table-responsive:last-child>.table:last-child>tbody:last-child>tr:last-child th:first-child,.panel>.table:last-child>tfoot:last-child>tr:last-child th:first-child,.panel>.table-responsive:last-child>.table:last-child>tfoot:last-child>tr:last-child th:first-child{border-bottom-left-radius:3px}.panel>.table:last-child>tbody:last-child>tr:last-child td:last-child,.panel>.table-responsive:last-child>.table:last-child>tbody:last-child>tr:last-child td:last-child,.panel>.table:last-child>tfoot:last-child>tr:last-child td:last-child,.panel>.table-responsive:last-child>.table:last-child>tfoot:last-child>tr:last-child td:last-child,.panel>.table:last-child>tbody:last-child>tr:last-child th:last-child,.panel>.table-responsive:last-child>.table:last-child>tbody:last-child>tr:last-child th:last-child,.panel>.table:last-child>tfoot:last-child>tr:last-child th:last-child,.panel>.table-responsive:last-child>.table:last-child>tfoot:last-child>tr:last-child th:last-child{border-bottom-right-radius:3px}.panel>.panel-body+.table,.panel>.panel-body+.table-responsive,.panel>.table+.panel-body,.panel>.table-responsive+.panel-body{border-top:1px solid #dddddd}.panel>.table>tbody:first-child>tr:first-child th,.panel>.table>tbody:first-child>tr:first-child td{border-top:0}.panel>.table-bordered,.panel>.table-responsive>.table-bordered{border:0}.panel>.table-bordered>thead>tr>th:first-child,.panel>.table-responsive>.table-bordered>thead>tr>th:first-child,.panel>.table-bordered>tbody>tr>th:first-child,.panel>.table-responsive>.table-bordered>tbody>tr>th:first-child,.panel>.table-bordered>tfoot>tr>th:first-child,.panel>.table-responsive>.table-bordered>tfoot>tr>th:first-child,.panel>.table-bordered>thead>tr>td:first-child,.panel>.table-responsive>.table-bordered>thead>tr>td:first-child,.panel>.table-bordered>tbody>tr>td:first-child,.panel>.table-responsive>.table-bordered>tbody>tr>td:first-child,.panel>.table-bordered>tfoot>tr>td:first-child,.panel>.table-responsive>.table-bordered>tfoot>tr>td:first-child{border-left:0}.panel>.table-bordered>thead>tr>th:last-child,.panel>.table-responsive>.table-bordered>thead>tr>th:last-child,.panel>.table-bordered>tbody>tr>th:last-child,.panel>.table-responsive>.table-bordered>tbody>tr>th:last-child,.panel>.table-bordered>tfoot>tr>th:last-child,.panel>.table-responsive>.table-bordered>tfoot>tr>th:last-child,.panel>.table-bordered>thead>tr>td:last-child,.panel>.table-responsive>.table-bordered>thead>tr>td:last-child,.panel>.table-bordered>tbody>tr>td:last-child,.panel>.table-responsive>.table-bordered>tbody>tr>td:last-child,.panel>.table-bordered>tfoot>tr>td:last-child,.panel>.table-responsive>.table-bordered>tfoot>tr>td:last-child{border-right:0}.panel>.table-bordered>thead>tr:first-child>td,.panel>.table-responsive>.table-bordered>thead>tr:first-child>td,.panel>.table-bordered>tbody>tr:first-child>td,.panel>.table-responsive>.table-bordered>tbody>tr:first-child>td,.panel>.table-bordered>thead>tr:first-child>th,.panel>.table-responsive>.table-bordered>thead>tr:first-child>th,.panel>.table-bordered>tbody>tr:first-child>th,.panel>.table-responsive>.table-bordered>tbody>tr:first-child>th{border-bottom:0}.panel>.table-bordered>tbody>tr:last-child>td,.panel>.table-responsive>.table-bordered>tbody>tr:last-child>td,.panel>.table-bordered>tfoot>tr:last-child>td,.panel>.table-responsive>.table-bordered>tfoot>tr:last-child>td,.panel>.table-bordered>tbody>tr:last-child>th,.panel>.table-responsive>.table-bordered>tbody>tr:last-child>th,.panel>.table-bordered>tfoot>tr:last-child>th,.panel>.table-responsive>.table-bordered>tfoot>tr:last-child>th{border-bottom:0}.panel>.table-responsive{border:0;margin-bottom:0}.panel-group{margin-bottom:20px}.panel-group .panel{margin-bottom:0;border-radius:4px}.panel-group .panel+.panel{margin-top:5px}.panel-group .panel-heading{border-bottom:0}.panel-group .panel-heading+.panel-collapse>.panel-body,.panel-group .panel-heading+.panel-collapse>.list-group{border-top:1px solid #dddddd}.panel-group .panel-footer{border-top:0}.panel-group .panel-footer+.panel-collapse .panel-body{border-bottom:1px solid #dddddd}.panel-default{border-color:#dddddd}.panel-default>.panel-heading{color:#555555;background-color:#f5f5f5;border-color:#dddddd}.panel-default>.panel-heading+.panel-collapse>.panel-body{border-top-color:#dddddd}.panel-default>.panel-heading .badge{color:#f5f5f5;background-color:#555555}.panel-default>.panel-footer+.panel-collapse>.panel-body{border-bottom-color:#dddddd}.panel-primary{border-color:#dddddd}.panel-primary>.panel-heading{color:#ffffff;background-color:#2fa4e7;border-color:#dddddd}.panel-primary>.panel-heading+.panel-collapse>.panel-body{border-top-color:#dddddd}.panel-primary>.panel-heading .badge{color:#2fa4e7;background-color:#ffffff}.panel-primary>.panel-footer+.panel-collapse>.panel-body{border-bottom-color:#dddddd}.panel-success{border-color:#dddddd}.panel-success>.panel-heading{color:#468847;background-color:#73a839;border-color:#dddddd}.panel-success>.panel-heading+.panel-collapse>.panel-body{border-top-color:#dddddd}.panel-success>.panel-heading .badge{color:#73a839;background-color:#468847}.panel-success>.panel-footer+.panel-collapse>.panel-body{border-bottom-color:#dddddd}.panel-info{border-color:#dddddd}.panel-info>.panel-heading{color:#3a87ad;background-color:#033c73;border-color:#dddddd}.panel-info>.panel-heading+.panel-collapse>.panel-body{border-top-color:#dddddd}.panel-info>.panel-heading .badge{color:#033c73;background-color:#3a87ad}.panel-info>.panel-footer+.panel-collapse>.panel-body{border-bottom-color:#dddddd}.panel-warning{border-color:#dddddd}.panel-warning>.panel-heading{color:#c09853;background-color:#dd5600;border-color:#dddddd}.panel-warning>.panel-heading+.panel-collapse>.panel-body{border-top-color:#dddddd}.panel-warning>.panel-heading .badge{color:#dd5600;background-color:#c09853}.panel-warning>.panel-footer+.panel-collapse>.panel-body{border-bottom-color:#dddddd}.panel-danger{border-color:#dddddd}.panel-danger>.panel-heading{color:#b94a48;background-color:#c71c22;border-color:#dddddd}.panel-danger>.panel-heading+.panel-collapse>.panel-body{border-top-color:#dddddd}.panel-danger>.panel-heading .badge{color:#c71c22;background-color:#b94a48}.panel-danger>.panel-footer+.panel-collapse>.panel-body{border-bottom-color:#dddddd}.embed-responsive{position:relative;display:block;height:0;padding:0;overflow:hidden}.embed-responsive .embed-responsive-item,.embed-responsive iframe,.embed-responsive embed,.embed-responsive object,.embed-responsive video{position:absolute;top:0;left:0;bottom:0;height:100%;width:100%;border:0}.embed-responsive-16by9{padding-bottom:56.25%}.embed-responsive-4by3{padding-bottom:75%}.well{min-height:20px;padding:19px;margin-bottom:20px;background-color:#f5f5f5;border:1px solid #e3e3e3;border-radius:4px;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.05);box-shadow:inset 0 1px 1px rgba(0,0,0,0.05)}.well blockquote{border-color:#ddd;border-color:rgba(0,0,0,0.15)}.well-lg{padding:24px;border-radius:6px}.well-sm{padding:9px;border-radius:3px}.close{float:right;font-size:21px;font-weight:bold;line-height:1;color:#000000;text-shadow:0 1px 0 #ffffff;opacity:0.2;filter:alpha(opacity=20)}.close:hover,.close:focus{color:#000000;text-decoration:none;cursor:pointer;opacity:0.5;filter:alpha(opacity=50)}button.close{padding:0;cursor:pointer;background:transparent;border:0;-webkit-appearance:none}.modal-open{overflow:hidden}.modal{display:none;overflow:hidden;position:fixed;top:0;right:0;bottom:0;left:0;z-index:1050;-webkit-overflow-scrolling:touch;outline:0}.modal.fade .modal-dialog{-webkit-transform:translate(0, -25%);-ms-transform:translate(0, -25%);-o-transform:translate(0, -25%);transform:translate(0, -25%);-webkit-transition:-webkit-transform .3s ease-out;-o-transition:-o-transform .3s ease-out;transition:transform .3s ease-out}.modal.in .modal-dialog{-webkit-transform:translate(0, 0);-ms-transform:translate(0, 0);-o-transform:translate(0, 0);transform:translate(0, 0)}.modal-open .modal{overflow-x:hidden;overflow-y:auto}.modal-dialog{position:relative;width:auto;margin:10px}.modal-content{position:relative;background-color:#ffffff;border:1px solid #999999;border:1px solid rgba(0,0,0,0.2);border-radius:6px;-webkit-box-shadow:0 3px 9px rgba(0,0,0,0.5);box-shadow:0 3px 9px rgba(0,0,0,0.5);-webkit-background-clip:padding-box;background-clip:padding-box;outline:0}.modal-backdrop{position:fixed;top:0;right:0;bottom:0;left:0;z-index:1040;background-color:#000000}.modal-backdrop.fade{opacity:0;filter:alpha(opacity=0)}.modal-backdrop.in{opacity:0.5;filter:alpha(opacity=50)}.modal-header{padding:15px;border-bottom:1px solid #e5e5e5}.modal-header .close{margin-top:-2px}.modal-title{margin:0;line-height:1.42857143}.modal-body{position:relative;padding:20px}.modal-footer{padding:20px;text-align:right;border-top:1px solid #e5e5e5}.modal-footer .btn+.btn{margin-left:5px;margin-bottom:0}.modal-footer .btn-group .btn+.btn{margin-left:-1px}.modal-footer .btn-block+.btn-block{margin-left:0}.modal-scrollbar-measure{position:absolute;top:-9999px;width:50px;height:50px;overflow:scroll}@media (min-width:768px){.modal-dialog{width:600px;margin:30px auto}.modal-content{-webkit-box-shadow:0 5px 15px rgba(0,0,0,0.5);box-shadow:0 5px 15px rgba(0,0,0,0.5)}.modal-sm{width:300px}}@media (min-width:992px){.modal-lg{width:900px}}.tooltip{position:absolute;z-index:1070;display:block;font-family:"Helvetica Neue",Helvetica,Arial,sans-serif;font-style:normal;font-weight:normal;letter-spacing:normal;line-break:auto;line-height:1.42857143;text-align:left;text-align:start;text-decoration:none;text-shadow:none;text-transform:none;white-space:normal;word-break:normal;word-spacing:normal;word-wrap:normal;font-size:12px;opacity:0;filter:alpha(opacity=0)}.tooltip.in{opacity:0.9;filter:alpha(opacity=90)}.tooltip.top{margin-top:-3px;padding:5px 0}.tooltip.right{margin-left:3px;padding:0 5px}.tooltip.bottom{margin-top:3px;padding:5px 0}.tooltip.left{margin-left:-3px;padding:0 5px}.tooltip-inner{max-width:200px;padding:3px 8px;color:#ffffff;text-align:center;background-color:#000000;border-radius:4px}.tooltip-arrow{position:absolute;width:0;height:0;border-color:transparent;border-style:solid}.tooltip.top .tooltip-arrow{bottom:0;left:50%;margin-left:-5px;border-width:5px 5px 0;border-top-color:#000000}.tooltip.top-left .tooltip-arrow{bottom:0;right:5px;margin-bottom:-5px;border-width:5px 5px 0;border-top-color:#000000}.tooltip.top-right .tooltip-arrow{bottom:0;left:5px;margin-bottom:-5px;border-width:5px 5px 0;border-top-color:#000000}.tooltip.right .tooltip-arrow{top:50%;left:0;margin-top:-5px;border-width:5px 5px 5px 0;border-right-color:#000000}.tooltip.left .tooltip-arrow{top:50%;right:0;margin-top:-5px;border-width:5px 0 5px 5px;border-left-color:#000000}.tooltip.bottom .tooltip-arrow{top:0;left:50%;margin-left:-5px;border-width:0 5px 5px;border-bottom-color:#000000}.tooltip.bottom-left .tooltip-arrow{top:0;right:5px;margin-top:-5px;border-width:0 5px 5px;border-bottom-color:#000000}.tooltip.bottom-right .tooltip-arrow{top:0;left:5px;margin-top:-5px;border-width:0 5px 5px;border-bottom-color:#000000}.popover{position:absolute;top:0;left:0;z-index:1060;display:none;max-width:276px;padding:1px;font-family:"Helvetica Neue",Helvetica,Arial,sans-serif;font-style:normal;font-weight:normal;letter-spacing:normal;line-break:auto;line-height:1.42857143;text-align:left;text-align:start;text-decoration:none;text-shadow:none;text-transform:none;white-space:normal;word-break:normal;word-spacing:normal;word-wrap:normal;font-size:14px;background-color:#ffffff;-webkit-background-clip:padding-box;background-clip:padding-box;border:1px solid #cccccc;border:1px solid rgba(0,0,0,0.2);border-radius:6px;-webkit-box-shadow:0 5px 10px rgba(0,0,0,0.2);box-shadow:0 5px 10px rgba(0,0,0,0.2)}.popover.top{margin-top:-10px}.popover.right{margin-left:10px}.popover.bottom{margin-top:10px}.popover.left{margin-left:-10px}.popover-title{margin:0;padding:8px 14px;font-size:14px;background-color:#f7f7f7;border-bottom:1px solid #ebebeb;border-radius:5px 5px 0 0}.popover-content{padding:9px 14px}.popover>.arrow,.popover>.arrow:after{position:absolute;display:block;width:0;height:0;border-color:transparent;border-style:solid}.popover>.arrow{border-width:11px}.popover>.arrow:after{border-width:10px;content:""}.popover.top>.arrow{left:50%;margin-left:-11px;border-bottom-width:0;border-top-color:#999999;border-top-color:rgba(0,0,0,0.25);bottom:-11px}.popover.top>.arrow:after{content:" ";bottom:1px;margin-left:-10px;border-bottom-width:0;border-top-color:#ffffff}.popover.right>.arrow{top:50%;left:-11px;margin-top:-11px;border-left-width:0;border-right-color:#999999;border-right-color:rgba(0,0,0,0.25)}.popover.right>.arrow:after{content:" ";left:1px;bottom:-10px;border-left-width:0;border-right-color:#ffffff}.popover.bottom>.arrow{left:50%;margin-left:-11px;border-top-width:0;border-bottom-color:#999999;border-bottom-color:rgba(0,0,0,0.25);top:-11px}.popover.bottom>.arrow:after{content:" ";top:1px;margin-left:-10px;border-top-width:0;border-bottom-color:#ffffff}.popover.left>.arrow{top:50%;right:-11px;margin-top:-11px;border-right-width:0;border-left-color:#999999;border-left-color:rgba(0,0,0,0.25)}.popover.left>.arrow:after{content:" ";right:1px;border-right-width:0;border-left-color:#ffffff;bottom:-10px}.carousel{position:relative}.carousel-inner{position:relative;overflow:hidden;width:100%}.carousel-inner>.item{display:none;position:relative;-webkit-transition:.6s ease-in-out left;-o-transition:.6s ease-in-out left;transition:.6s ease-in-out left}.carousel-inner>.item>img,.carousel-inner>.item>a>img{line-height:1}@media all and (transform-3d),(-webkit-transform-3d){.carousel-inner>.item{-webkit-transition:-webkit-transform .6s ease-in-out;-o-transition:-o-transform .6s ease-in-out;transition:transform .6s ease-in-out;-webkit-backface-visibility:hidden;backface-visibility:hidden;-webkit-perspective:1000px;perspective:1000px}.carousel-inner>.item.next,.carousel-inner>.item.active.right{-webkit-transform:translate3d(100%, 0, 0);transform:translate3d(100%, 0, 0);left:0}.carousel-inner>.item.prev,.carousel-inner>.item.active.left{-webkit-transform:translate3d(-100%, 0, 0);transform:translate3d(-100%, 0, 0);left:0}.carousel-inner>.item.next.left,.carousel-inner>.item.prev.right,.carousel-inner>.item.active{-webkit-transform:translate3d(0, 0, 0);transform:translate3d(0, 0, 0);left:0}}.carousel-inner>.active,.carousel-inner>.next,.carousel-inner>.prev{display:block}.carousel-inner>.active{left:0}.carousel-inner>.next,.carousel-inner>.prev{position:absolute;top:0;width:100%}.carousel-inner>.next{left:100%}.carousel-inner>.prev{left:-100%}.carousel-inner>.next.left,.carousel-inner>.prev.right{left:0}.carousel-inner>.active.left{left:-100%}.carousel-inner>.active.right{left:100%}.carousel-control{position:absolute;top:0;left:0;bottom:0;width:15%;opacity:0.5;filter:alpha(opacity=50);font-size:20px;color:#ffffff;text-align:center;text-shadow:0 1px 2px rgba(0,0,0,0.6);background-color:rgba(0,0,0,0)}.carousel-control.left{background-image:-webkit-linear-gradient(left, rgba(0,0,0,0.5) 0, rgba(0,0,0,0.0001) 100%);background-image:-o-linear-gradient(left, rgba(0,0,0,0.5) 0, rgba(0,0,0,0.0001) 100%);background-image:-webkit-gradient(linear, left top, right top, from(rgba(0,0,0,0.5)), to(rgba(0,0,0,0.0001)));background-image:linear-gradient(to right, rgba(0,0,0,0.5) 0, rgba(0,0,0,0.0001) 100%);background-repeat:repeat-x;filter:progid:DXImageTransform.Microsoft.gradient(startColorstr='#80000000', endColorstr='#00000000', GradientType=1)}.carousel-control.right{left:auto;right:0;background-image:-webkit-linear-gradient(left, rgba(0,0,0,0.0001) 0, rgba(0,0,0,0.5) 100%);background-image:-o-linear-gradient(left, rgba(0,0,0,0.0001) 0, rgba(0,0,0,0.5) 100%);background-image:-webkit-gradient(linear, left top, right top, from(rgba(0,0,0,0.0001)), to(rgba(0,0,0,0.5)));background-image:linear-gradient(to right, rgba(0,0,0,0.0001) 0, rgba(0,0,0,0.5) 100%);background-repeat:repeat-x;filter:progid:DXImageTransform.Microsoft.gradient(startColorstr='#00000000', endColorstr='#80000000', GradientType=1)}.carousel-control:hover,.carousel-control:focus{outline:0;color:#ffffff;text-decoration:none;opacity:0.9;filter:alpha(opacity=90)}.carousel-control .icon-prev,.carousel-control .icon-next,.carousel-control .glyphicon-chevron-left,.carousel-control .glyphicon-chevron-right{position:absolute;top:50%;margin-top:-10px;z-index:5;display:inline-block}.carousel-control .icon-prev,.carousel-control .glyphicon-chevron-left{left:50%;margin-left:-10px}.carousel-control .icon-next,.carousel-control .glyphicon-chevron-right{right:50%;margin-right:-10px}.carousel-control .icon-prev,.carousel-control .icon-next{width:20px;height:20px;line-height:1;font-family:serif}.carousel-control .icon-prev:before{content:'\2039'}.carousel-control .icon-next:before{content:'\203a'}.carousel-indicators{position:absolute;bottom:10px;left:50%;z-index:15;width:60%;margin-left:-30%;padding-left:0;list-style:none;text-align:center}.carousel-indicators li{display:inline-block;width:10px;height:10px;margin:1px;text-indent:-999px;border:1px solid #ffffff;border-radius:10px;cursor:pointer;background-color:#000 \9;background-color:rgba(0,0,0,0)}.carousel-indicators .active{margin:0;width:12px;height:12px;background-color:#ffffff}.carousel-caption{position:absolute;left:15%;right:15%;bottom:20px;z-index:10;padding-top:20px;padding-bottom:20px;color:#ffffff;text-align:center;text-shadow:0 1px 2px rgba(0,0,0,0.6)}.carousel-caption .btn{text-shadow:none}@media screen and (min-width:768px){.carousel-control .glyphicon-chevron-left,.carousel-control .glyphicon-chevron-right,.carousel-control .icon-prev,.carousel-control .icon-next{width:30px;height:30px;margin-top:-10px;font-size:30px}.carousel-control .glyphicon-chevron-left,.carousel-control .icon-prev{margin-left:-10px}.carousel-control .glyphicon-chevron-right,.carousel-control .icon-next{margin-right:-10px}.carousel-caption{left:20%;right:20%;padding-bottom:30px}.carousel-indicators{bottom:20px}}.clearfix:before,.clearfix:after,.dl-horizontal dd:before,.dl-horizontal dd:after,.container:before,.container:after,.container-fluid:before,.container-fluid:after,.row:before,.row:after,.form-horizontal .form-group:before,.form-horizontal .form-group:after,.btn-toolbar:before,.btn-toolbar:after,.btn-group-vertical>.btn-group:before,.btn-group-vertical>.btn-group:after,.nav:before,.nav:after,.navbar:before,.navbar:after,.navbar-header:before,.navbar-header:after,.navbar-collapse:before,.navbar-collapse:after,.pager:before,.pager:after,.panel-body:before,.panel-body:after,.modal-header:before,.modal-header:after,.modal-footer:before,.modal-footer:after{content:" ";display:table}.clearfix:after,.dl-horizontal dd:after,.container:after,.container-fluid:after,.row:after,.form-horizontal .form-group:after,.btn-toolbar:after,.btn-group-vertical>.btn-group:after,.nav:after,.navbar:after,.navbar-header:after,.navbar-collapse:after,.pager:after,.panel-body:after,.modal-header:after,.modal-footer:after{clear:both}.center-block{display:block;margin-left:auto;margin-right:auto}.pull-right{float:right !important}.pull-left{float:left !important}.hide{display:none !important}.show{display:block !important}.invisible{visibility:hidden}.text-hide{font:0/0 a;color:transparent;text-shadow:none;background-color:transparent;border:0}.hidden{display:none !important}.affix{position:fixed}@-ms-viewport{width:device-width}.visible-xs,.visible-sm,.visible-md,.visible-lg{display:none !important}.visible-xs-block,.visible-xs-inline,.visible-xs-inline-block,.visible-sm-block,.visible-sm-inline,.visible-sm-inline-block,.visible-md-block,.visible-md-inline,.visible-md-inline-block,.visible-lg-block,.visible-lg-inline,.visible-lg-inline-block{display:none !important}@media (max-width:767px){.visible-xs{display:block !important}table.visible-xs{display:table !important}tr.visible-xs{display:table-row !important}th.visible-xs,td.visible-xs{display:table-cell !important}}@media (max-width:767px){.visible-xs-block{display:block !important}}@media (max-width:767px){.visible-xs-inline{display:inline !important}}@media (max-width:767px){.visible-xs-inline-block{display:inline-block !important}}@media (min-width:768px) and (max-width:991px){.visible-sm{display:block !important}table.visible-sm{display:table !important}tr.visible-sm{display:table-row !important}th.visible-sm,td.visible-sm{display:table-cell !important}}@media (min-width:768px) and (max-width:991px){.visible-sm-block{display:block !important}}@media (min-width:768px) and (max-width:991px){.visible-sm-inline{display:inline !important}}@media (min-width:768px) and (max-width:991px){.visible-sm-inline-block{display:inline-block !important}}@media (min-width:992px) and (max-width:1199px){.visible-md{display:block !important}table.visible-md{display:table !important}tr.visible-md{display:table-row !important}th.visible-md,td.visible-md{display:table-cell !important}}@media (min-width:992px) and (max-width:1199px){.visible-md-block{display:block !important}}@media (min-width:992px) and (max-width:1199px){.visible-md-inline{display:inline !important}}@media (min-width:992px) and (max-width:1199px){.visible-md-inline-block{display:inline-block !important}}@media (min-width:1200px){.visible-lg{display:block !important}table.visible-lg{display:table !important}tr.visible-lg{display:table-row !important}th.visible-lg,td.visible-lg{display:table-cell !important}}@media (min-width:1200px){.visible-lg-block{display:block !important}}@media (min-width:1200px){.visible-lg-inline{display:inline !important}}@media (min-width:1200px){.visible-lg-inline-block{display:inline-block !important}}@media (max-width:767px){.hidden-xs{display:none !important}}@media (min-width:768px) and (max-width:991px){.hidden-sm{display:none !important}}@media (min-width:992px) and (max-width:1199px){.hidden-md{display:none !important}}@media (min-width:1200px){.hidden-lg{display:none !important}}.visible-print{display:none !important}@media print{.visible-print{display:block !important}table.visible-print{display:table !important}tr.visible-print{display:table-row !important}th.visible-print,td.visible-print{display:table-cell !important}}.visible-print-block{display:none !important}@media print{.visible-print-block{display:block !important}}.visible-print-inline{display:none !important}@media print{.visible-print-inline{display:inline !important}}.visible-print-inline-block{display:none !important}@media print{.visible-print-inline-block{display:inline-block !important}}@media print{.hidden-print{display:none !important}}.navbar-default{background-image:-webkit-linear-gradient(#54b4eb, #2fa4e7 60%, #1d9ce5);background-image:-o-linear-gradient(#54b4eb, #2fa4e7 60%, #1d9ce5);background-image:-webkit-gradient(linear, left top, left bottom, from(#54b4eb), color-stop(60%, #2fa4e7), to(#1d9ce5));background-image:linear-gradient(#54b4eb, #2fa4e7 60%, #1d9ce5);background-repeat:no-repeat;filter:progid:DXImageTransform.Microsoft.gradient(startColorstr='#ff54b4eb', endColorstr='#ff1d9ce5', GradientType=0);border-bottom:1px solid #178acc;-webkit-filter:none;filter:none;-webkit-box-shadow:0 1px 10px rgba(0,0,0,0.1);box-shadow:0 1px 10px rgba(0,0,0,0.1)}.navbar-default .badge{background-color:#fff;color:#2fa4e7}.navbar-inverse{background-image:-webkit-linear-gradient(#04519b, #044687 60%, #033769);background-image:-o-linear-gradient(#04519b, #044687 60%, #033769);background-image:-webkit-gradient(linear, left top, left bottom, from(#04519b), color-stop(60%, #044687), to(#033769));background-image:linear-gradient(#04519b, #044687 60%, #033769);background-repeat:no-repeat;filter:progid:DXImageTransform.Microsoft.gradient(startColorstr='#ff04519b', endColorstr='#ff033769', GradientType=0);-webkit-filter:none;filter:none;border-bottom:1px solid #022241}.navbar-inverse .badge{background-color:#fff;color:#033c73}.navbar .navbar-nav>li>a,.navbar-brand{text-shadow:0 1px 0 rgba(0,0,0,0.1)}@media (max-width:767px){.navbar .dropdown-header{color:#fff}.navbar .dropdown-menu a{color:#fff}}.btn{text-shadow:0 1px 0 rgba(0,0,0,0.1)}.btn .caret{border-top-color:#fff}.btn-default{background-image:-webkit-linear-gradient(#fff, #fff 60%, #f5f5f5);background-image:-o-linear-gradient(#fff, #fff 60%, #f5f5f5);background-image:-webkit-gradient(linear, left top, left bottom, from(#fff), color-stop(60%, #fff), to(#f5f5f5));background-image:linear-gradient(#fff, #fff 60%, #f5f5f5);background-repeat:no-repeat;filter:progid:DXImageTransform.Microsoft.gradient(startColorstr='#ffffffff', endColorstr='#fff5f5f5', GradientType=0);-webkit-filter:none;filter:none;border-bottom:1px solid #e6e6e6}.btn-default:hover{color:#555555}.btn-default .caret{border-top-color:#555555}.btn-default{background-image:-webkit-linear-gradient(#fff, #fff 60%, #f5f5f5);background-image:-o-linear-gradient(#fff, #fff 60%, #f5f5f5);background-image:-webkit-gradient(linear, left top, left bottom, from(#fff), color-stop(60%, #fff), to(#f5f5f5));background-image:linear-gradient(#fff, #fff 60%, #f5f5f5);background-repeat:no-repeat;filter:progid:DXImageTransform.Microsoft.gradient(startColorstr='#ffffffff', endColorstr='#fff5f5f5', GradientType=0);-webkit-filter:none;filter:none;border-bottom:1px solid #e6e6e6}.btn-primary{background-image:-webkit-linear-gradient(#54b4eb, #2fa4e7 60%, #1d9ce5);background-image:-o-linear-gradient(#54b4eb, #2fa4e7 60%, #1d9ce5);background-image:-webkit-gradient(linear, left top, left bottom, from(#54b4eb), color-stop(60%, #2fa4e7), to(#1d9ce5));background-image:linear-gradient(#54b4eb, #2fa4e7 60%, #1d9ce5);background-repeat:no-repeat;filter:progid:DXImageTransform.Microsoft.gradient(startColorstr='#ff54b4eb', endColorstr='#ff1d9ce5', GradientType=0);-webkit-filter:none;filter:none;border-bottom:1px solid #178acc}.btn-success{background-image:-webkit-linear-gradient(#88c149, #73a839 60%, #699934);background-image:-o-linear-gradient(#88c149, #73a839 60%, #699934);background-image:-webkit-gradient(linear, left top, left bottom, from(#88c149), color-stop(60%, #73a839), to(#699934));background-image:linear-gradient(#88c149, #73a839 60%, #699934);background-repeat:no-repeat;filter:progid:DXImageTransform.Microsoft.gradient(startColorstr='#ff88c149', endColorstr='#ff699934', GradientType=0);-webkit-filter:none;filter:none;border-bottom:1px solid #59822c}.btn-info{background-image:-webkit-linear-gradient(#04519b, #033c73 60%, #02325f);background-image:-o-linear-gradient(#04519b, #033c73 60%, #02325f);background-image:-webkit-gradient(linear, left top, left bottom, from(#04519b), color-stop(60%, #033c73), to(#02325f));background-image:linear-gradient(#04519b, #033c73 60%, #02325f);background-repeat:no-repeat;filter:progid:DXImageTransform.Microsoft.gradient(startColorstr='#ff04519b', endColorstr='#ff02325f', GradientType=0);-webkit-filter:none;filter:none;border-bottom:1px solid #022241}.btn-warning{background-image:-webkit-linear-gradient(#ff6707, #dd5600 60%, #c94e00);background-image:-o-linear-gradient(#ff6707, #dd5600 60%, #c94e00);background-image:-webkit-gradient(linear, left top, left bottom, from(#ff6707), color-stop(60%, #dd5600), to(#c94e00));background-image:linear-gradient(#ff6707, #dd5600 60%, #c94e00);background-repeat:no-repeat;filter:progid:DXImageTransform.Microsoft.gradient(startColorstr='#ffff6707', endColorstr='#ffc94e00', GradientType=0);-webkit-filter:none;filter:none;border-bottom:1px solid #aa4200}.btn-danger{background-image:-webkit-linear-gradient(#e12b31, #c71c22 60%, #b5191f);background-image:-o-linear-gradient(#e12b31, #c71c22 60%, #b5191f);background-image:-webkit-gradient(linear, left top, left bottom, from(#e12b31), color-stop(60%, #c71c22), to(#b5191f));background-image:linear-gradient(#e12b31, #c71c22 60%, #b5191f);background-repeat:no-repeat;filter:progid:DXImageTransform.Microsoft.gradient(startColorstr='#ffe12b31', endColorstr='#ffb5191f', GradientType=0);-webkit-filter:none;filter:none;border-bottom:1px solid #9a161a}.panel-primary .panel-heading,.panel-success .panel-heading,.panel-warning .panel-heading,.panel-danger .panel-heading,.panel-info .panel-heading,.panel-primary .panel-title,.panel-success .panel-title,.panel-warning .panel-title,.panel-danger .panel-title,.panel-info .panel-title{color:#fff}
)";

static const std::string dark_theme_css =
    R"(
		body {
  margin-top: 60px;
  color: #a4a4a4;
  background-color: #131313;
}

a {
    color: #4C7FA6;
    text-decoration: none;
}

.nav .btn-default {
    color: #A4A4A4;
    background-color: #1D2E4B;
    border-color: #1D2E4B;
}

.form-control {
    color: #A4A4A4;
    background-color: #13171E;
	border: 1px solid #1D2E4B;
}


.nav .form-control,
.input-group .form-control {
    border: 1px solid #1D2E4B;
}

.nav input,
.input-group input {
	background-color: #13171E;
	color: #A4A4A4;
}

.nav input::placeholder,
.input-group input::placeholder {
	color: #474F5D;
}

.input-group-addon {
	border: 1px solid #1D2E4B;
	background-color: #1A2539;
	color: #A4A4A4;
}

.table > thead > tr > td.success,
.table > tbody > tr > td.success,
.table > tfoot > tr > td.success,
.table > thead > tr > th.success,
.table > tbody > tr > th.success,
.table > tfoot > tr > th.success,
.table > thead > tr.success > td,
.table > tbody > tr.success > td,
.table > tfoot > tr.success > td,
.table > thead > tr.success > th,
.table > tbody > tr.success > th,
.table > tfoot > tr.success > th {
  background-color: #002B36;
}

.table-hover > tbody > tr:hover {
  background-color: #353E4E;
}

.table-hover > tbody > tr > td:hover {
  background-color: #2D384C;
}

.table-hover > tbody > tr > td.success:hover,
.table-hover > tbody > tr > th.success:hover,
.table-hover > tbody > tr.success:hover > td,
.table-hover > tbody > tr:hover > .success,
.table-hover > tbody > tr.success:hover > th {
  background-color: #073642;
}
.table > thead > tr > th {
  color: #336A80;
  border-bottom-color: #336A80;
  text-align: center;
  font-size: 1em;
}
.table > tbody > tr > td {
  border-top-color: #1D2C47;  
  vertical-align: middle;
  font-family: 'Inconsolata', monospace;
  font-size: 1em;
  text-align: center;
  padding: 8px 2px;
}

td .mtx-ago,
td .timeago {
	font-size: 0.9em;
}

.table>thead>tr>td.danger, .table>tbody>tr>td.danger, .table>tfoot>tr>td.danger, .table>thead>tr>th.danger, .table>tbody>tr>th.danger, .table>tfoot>tr>th.danger, .table>thead>tr.danger>td, .table>tbody>tr.danger>td, .table>tfoot>tr.danger>td, .table>thead>tr.danger>th, .table>tbody>tr.danger>th, .table>tfoot>tr.danger>th {
    background-color: #280006!important;
}

.table>thead>tr>td.warning, .table>tbody>tr>td.warning, .table>tfoot>tr>td.warning, .table>thead>tr>th.warning, .table>tbody>tr>th.warning, .table>tfoot>tr>th.warning, .table>thead>tr.warning>td, .table>tbody>tr.warning>td, .table>tfoot>tr.warning>td, .table>thead>tr.warning>th, .table>tbody>tr.warning>th, .table>tfoot>tr.warning>th,
.table-hover > tbody > tr > td.warning:hover,
.table-hover > tbody > tr > th.warning:hover,
.table-hover > tbody > tr.warning:hover > td,
.table-hover > tbody > tr:hover > .warning,
.table-hover > tbody > tr.warning:hover > th
{
    background-color: #231C13;
}

#mem_pool_table td {
	text-align: center;
}

#blocks_rows > tr > td:nth-child(2) {
	text-align: left;
}

#blocks_rows > tr > td:nth-child(4) {
	/*font-size: 85%;*/
}

#content {
	margin-bottom: 300px;
	z-index: 1;
	background-color: #0C0D0F;
}

footer {
  background-color: #0C0D0F;
}

 /* The switch - the box around the slider */
.switch {
  position: relative;
  display: block;
  width: 60px;
  height: 34px;
  margin: 8px 0;
}

/* Hide default HTML checkbox */
.switch input {
  opacity: 0;
  width: 0;
  height: 0;
}

/* The slider */
.switch-slider {
  position: absolute;
  cursor: pointer;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background-color: #ccc;
  -webkit-transition: .4s;
  transition: .4s;
}

.switch-slider:before {
  font-family: FontAwesome;
  position: absolute;
  content: "\f185";
  font-size: 18px;
  line-height: 26px;
  text-align: center;
  color: #A4A4A4;
  height: 26px;
  width: 26px;
  left: 4px;
  bottom: 4px;
  background-color: #2B3951;
  -webkit-transition: .4s;
  transition: .4s;
}

input:checked + .switch-slider {
  background-color: #13171E;
}

input:focus + .switch-slider {
  box-shadow: 0 0 1px #13171E;
}

input:checked + .switch-slider:before {
  -webkit-transform: translateX(26px);
  -ms-transform: translateX(26px);
  transform: translateX(26px);
}

/* Rounded sliders */
.switch-slider.round {
  border-radius: 34px;
}

.switch-slider.round:before {
  border-radius: 50%;
} 


.panel, .panel-default, .list-group-item {
	background-color: #13171E;
	border-color: #1D2C47;
}

.panel-default > .panel-heading {
	background-color: #192740;
	color: #F2F2F2;
	border-bottom-color: #1D2C47;
}


.navbar-default {
    background-image: -webkit-linear-gradient(#192740, #182235 60%, #13171E);
    background-image: -o-linear-gradient(#192740, #182235 60%, #13171E);
    background-image: -webkit-gradient(linear, left top, left bottom, from(#192740), color-stop(60%, #182235), to(#13171E));
    background-image: linear-gradient(#192740, #182235 60%, #13171E);
    background-repeat: no-repeat;
    filter: progid:DXImageTransform.Microsoft.gradient(startColorstr='#ff54b4eb', endColorstr='#ff1d9ce5', GradientType=0);
    border-bottom: 1px solid #1D2E4B;
    -webkit-filter: none;
    filter: none;
    -webkit-box-shadow: 0 1px 10px rgba(0,0,0,0.1);
    box-shadow: 0 1px 10px rgba(0,0,0,0.1);
}

.navbar-default .navbar-nav > li > a:hover,
.navbar-default .navbar-nav > li > a:focus {
    color: #A4A4A4;
    background-color: #1D2E4B;
}

.navbar-default .navbar-nav > .active > a,
.navbar-default .navbar-nav > .active > a:hover,
.navbar-default .navbar-nav > .active > a:focus {
    color: #A4A4A4;
    background-color: #1D2E4B;
}

.nav-tabs {

    border-bottom: 2px solid #1D2C47;

}

.nav-tabs > li > a:hover,
.nav-tabs > li > a:focus,
.nav-tabs > li.active > a,
.nav-tabs > li.active > a:hover,
.nav-tabs > li.active > a:focus {
    color: #A4A4A4;
    background-color: #192740;
    border: 1px solid #1D2C47;
    border-bottom-color: #1D2C47;
    cursor: default;
}

.btn, .btn-default {
    color: #A4A4A4;
    background: #1D2C47;
    border-color: #1D2C47;
	border: 1px solid #1D2C47;
	border-radius: 5px;
}

.btn-default {
    background: #1D2C47;
}

.btn:active,
.btn:hover,
.btn:focus {
	border-color: #1D2E4B;
	color: #fff;
	background: #13171E;
}

.btn-default.disabled:hover, .btn-default[disabled]:hover, fieldset[disabled] .btn-default:hover, .btn-default.disabled:focus, .btn-default[disabled]:focus, fieldset[disabled] .btn-default:focus, .btn-default.disabled.focus, .btn-default[disabled].focus, fieldset[disabled] .btn-default.focus {
    background-color: #1A2539;
    border-color: #1A2539;
}

.well {
    min-height: 20px;
    padding: 19px;
    margin-bottom: 20px;
    background-color: #13171E;
    border: 1px solid #1D2C47;
    border-radius: 4px;
    -webkit-box-shadow: inset 0 1px 1px rgba(0,0,0,0.1);
    box-shadow: inset 0 1px 1px rgba(0,0,0,0.1);
}


.list-group-item.active, .list-group-item.active:hover, .list-group-item.active:focus {
    color: #A4A4A4;
    background-color: #1D2C47;
    border-color: #1A2539;
}

.panel-primary > .panel-heading {
    color: #A4A4A4;
    background-color: #1D2C47;
    border-color: #1A2539;
}

.panel-primary .panel-title {
	color: #A4A4A4;
}
	)";

	static const std::string default_theme_css =
    R"(@import url(//fonts.googleapis.com/css?family=Roboto+Condensed:400,700);
@import url(//fonts.googleapis.com/css?family=Roboto:400,300,500);
@import url("bootstrap.min.css");

#coinName{
    text-transform: capitalize;
    }

#coinIcon{
    text-transform: capitalize;
	width: 1.3em;
	height: 1.3em;
	display: inline-block;
	vertical-align: middle;
	color: #FFF137;
	border: 2px solid #FFF137;
	border-radius: 50%;
	text-align: center;
	margin-top: -4px;
}

body {
  margin-top: 60px;
}

.navbar-inverse {
  background-color: #2D5768;
  border-color: #f9f9f8;
  border-width: 0;
}
.navbar-inverse .container {
  background-color: rgba(0, 0, 0, 0);
}
.navbar-inverse .navbar-brand {
  color: #fff;
}
.navbar-inverse .navbar-brand:hover,
.navbar-inverse .navbar-brand:focus {
  color: #fff;
  background-color: rgba(255, 255, 255, 0);
}
.navbar-inverse .navbar-text {
  color: #CEE3E4;
}
.navbar-inverse .navbar-nav > li > a {
  color: #F0F9FA;
}
.navbar-inverse .navbar-nav > li > a:hover,
.navbar-inverse .navbar-nav > li > a:focus {
  color: #ffffff;
  background-color: transparent;
}
.navbar-inverse .navbar-nav > .active > a,
.navbar-inverse .navbar-nav > .active > a:hover,
.navbar-inverse .navbar-nav > .active > a:focus {
  color: #ffffff;
  background-color: #00A0E3;
}

.navbar-default .navbar-nav>li>a .fa,
.navbar-default .navbar-nav>.active>a .fa,
.navbar-inverse .fa {
	color: #FFF137;
}


.input-group .form-control {
	border-left: 0;
	border-right: 0;
}

#stats_updated {
  color: #F4FC3D;
  position: absolute; 
  right: 10px;
  top: 15px;
}
hr {
  border-top-color: #BBBBBB;
}
.stats > div {
  color: #7C7C7C;
}

.stats > div i.fa {
  color: #00A0E3;
  font-size: 21px;
  width: 30px;
  text-align: center;
}
.stats > div > span {
  font-weight: 500;
  padding: 0 2px;
}
.nav .form-control {
  border: 1px solid #00A0E3;
  border-radius: 4px;
  -webkit-box-shadow: inset 0 1px 1px rgba(0, 0, 0, 0.075);
  box-shadow: inset 0 1px 1px rgba(0, 0, 0, 0.075);
  -webkit-transition: border-color ease-in-out .15s, box-shadow ease-in-out .15s;
  -o-transition: border-color ease-in-out .15s, box-shadow ease-in-out .15s;
  transition: border-color ease-in-out .15s, box-shadow ease-in-out .15s;
}
.nav .form-control:focus {
  -webkit-box-shadow: none;
  box-shadow: none;
  border-color: #178ACC;
}
.nav .input-group-addon {
  background-color: #00A0E3;
  color: #fff;
  border-color: #00A0E3;
}
.nav .btn-default {
  color: #ffffff;
  background-color: #00A0E3;
  border-color: #00A0E3;
}
.nav .btn-default:hover,
.nav .btn-default:focus,
.nav .btn-default:active,
.nav .btn-default.active,
.nav .open > .dropdown-toggle.btn-default {
  color: #ffffff;
  background-color: #178ACC;
  border-color: #026a4d;
}
.nav .btn-default:active,
.nav .btn-default.active,
.nav .open > .dropdown-toggle.btn-default {
  background-image: none;
}
.nav .btn-default.disabled,
.nav .btn-default[disabled],
.nav fieldset[disabled] .btn-default,
.nav .btn-default.disabled:hover,
.nav .btn-default[disabled]:hover,
.nav fieldset[disabled] .btn-default:hover,
.nav .btn-default.disabled:focus,
.nav .btn-default[disabled]:focus,
.nav fieldset[disabled] .btn-default:focus,
.nav .btn-default.disabled:active,
.nav .btn-default[disabled]:active,
.nav fieldset[disabled] .btn-default:active,
.nav .btn-default.disabled.active,
.nav .btn-default[disabled].active,
.nav fieldset[disabled] .btn-default.active {
  background-color: #00A0E3;
  border-color: #00A0E3;
}
.nav .btn-default .badge {
  color: #00A0E3;
  background-color: #ffffff;
}

.navbar .theme-switch {
	background: transparent;
	border: none;
	line-height: 20px;
	padding-top: 15px;
    padding-bottom: 15px;
	border-radius: 0;
}

code {
  padding: 2px 10px;
  color: #DB2B24;
  background-color: #FDECF1;
  border-radius: 0;
}
.container .paymentsStatHolder,
.container .blocksStatHolder {
  padding-top: 10px;
}
.container .paymentsStatHolder > span,
.container .blocksStatHolder > span {
  border-width: 0;
  padding: 6px 13px;
}
.container .paymentsStatHolder > span > span,
.container .blocksStatHolder > span > span {
  font-weight: 500;
}
.bg-primary {
  background-color: #00A0E3;
}
.bg-info {
  background-color: #336A80;
  color: #fff;
}

.table > thead > tr > td.success,
.table > tbody > tr > td.success,
.table > tfoot > tr > td.success,
.table > thead > tr > th.success,
.table > tbody > tr > th.success,
.table > tfoot > tr > th.success,
.table > thead > tr.success > td,
.table > tbody > tr.success > td,
.table > tfoot > tr.success > td,
.table > thead > tr.success > th,
.table > tbody > tr.success > th,
.table > tfoot > tr.success > th {
  background-color: #C3F0E0;
}
.table-hover > tbody > tr > td.success:hover,
.table-hover > tbody > tr > th.success:hover,
.table-hover > tbody > tr.success:hover > td,
.table-hover > tbody > tr:hover > .success,
.table-hover > tbody > tr.success:hover > th {
  background-color: #ADDABB;
}
.table > thead > tr > th {
  color: #336A80;
  border-bottom-color: #336A80;
  text-align: center;
  font-size: 1em;
}
.table > tbody > tr > td {
  border-top-color: #c9e0e9;  
  vertical-align: middle;
  font-family: 'Inconsolata', monospace;
  font-size: 1em;
  text-align: center;
  padding: 8px 2px;
}

td .mtx-ago,
td .timeago {
	font-size: 0.9em;
}

.table>thead>tr>td.danger, .table>tbody>tr>td.danger, .table>tfoot>tr>td.danger, .table>thead>tr>th.danger, .table>tbody>tr>th.danger, .table>tfoot>tr>th.danger, .table>thead>tr.danger>td, .table>tbody>tr.danger>td, .table>tfoot>tr.danger>td, .table>thead>tr.danger>th, .table>tbody>tr.danger>th, .table>tfoot>tr.danger>th {
    background-color: #D7A4A4!important;
}

#mem_pool_table td {
	text-align: center;
}

#blocks_rows > tr > td:nth-child(2) {
	text-align: left;
}

#blocks_rows > tr > td:nth-child(4) {
	/*font-size: 85%;*/
}

#content {
	margin-bottom: 300px;
	z-index: 1;
	background-color: #FDFDFD;
}

footer {
  background-color: #333333;
  color: #FDFDFD;
  position: fixed;
  bottom: 0;
  z-index: -1;
  padding: 30px 0;
  width: 100%;
  height: 300px;
}

.scrollup{
	opacity:0.3;
	position:fixed;
	bottom:50px;
	right:100px;
	display:none;			
	text-align: center;
	line-height: 50px;
	font-size: 50px;
	-webkit-transition: color 200ms ease-in-out;
	-moz-transition:    color 200ms ease-in-out;
	-o-transition:      color 200ms ease-in-out;
	-ms-transition: 	color 200ms ease-out;
	display: none;
	overflow: hidden;
}

#block.hash, #transaction.hash {
	word-break: break-all;
}

/************************************************
*** sparkline override ***
************************************************/
.jqstooltip {
  border: none !important;
  background: rgba(0, 0, 0, 0.8) !important;
  border-radius: 2px !important;
  margin-top: -20px !important;
  /* Override Bootstrap defaults to fix jQuery.Sparkline tooltips appearance */
  -webkit-box-sizing: content-box !important;
  -moz-box-sizing: content-box;
  box-sizing: content-box !important;
}
.jqstooltip .jqsfield {
  color: #ccc;
  font-size: 13px !important;
}
.jqstooltip .jqsfield b {
  color: #fff;
}


strong,
b {
  font-weight: 700;
}
.nav-pills > li > a {
  background-color: #F3F3F3;
}
.nav-pills > li.active > a,
.nav-pills > li.active > a:hover,
.nav-pills > li.active > a:focus {
  color: #fff;
  background-color: #00A0E3;
}


.input-group-btn:last-child > .btn, .input-group-btn:last-child > .btn-group {
    z-index: 2;
    margin-left: -1px;
}

.input-group-btn:last-child > .btn {
    display: inline-block;
    margin-bottom: 0;
    font-weight: normal;
    text-align: center;
    vertical-align: middle;
    -ms-touch-action: manipulation;
    touch-action: manipulation;
    cursor: pointer;
    background-image: none;
    border: 1px solid transparent;
    white-space: nowrap;
    padding: 8px 12px;
    font-size: 14px;
    line-height: 1.42857143;
    border-radius: 4px;
    -webkit-user-select: none;
    -moz-user-select: none;
    -ms-user-select: none;
    user-select: none;
	border-bottom-left-radius: 0;
    border-top-left-radius: 0;
	border-bottom-right-radius: 4px;
    border-top-right-radius: 4px;
}

.explorer-search {
	margin-top: 6px;
}

.blocksStatHolder > span{
    display: inline-block;
    border-radius: 5px;
    padding: 1px 9px;
    border: 1px solid #e5e5e5;
    margin: 2px;
}
	
.blocksStatHolder > span > span{
    font-weight: bold;
}

.warning,
.text-warning {
	color: #FFC000;
}




 /* The switch - the box around the slider */
.switch {
  position: relative;
  display: inline-block;
  width: 60px;
  height: 34px;
  margin: 8px 0;
}

/* Hide default HTML checkbox */
.switch input {
  opacity: 0;
  width: 0;
  height: 0;
}

/* The slider */
.switch-slider {
  position: absolute;
  cursor: pointer;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background-color: #178ACC;
  -webkit-transition: .4s;
  transition: .4s;
}

.switch-slider:before {
  font-family: FontAwesome;
  position: absolute;
  content: "\f186";
  font-size: 18px;
  line-height: 26px;
  text-align: center;
  color: #178ACC;
  height: 26px;
  width: 26px;
  left: 4px;
  bottom: 4px;
  background-color: white;
  -webkit-transition: .4s;
  transition: .4s;
}

input:checked + .switch-slider {
  background-color: #2196F3;
}

input:focus + .switch-slider {
  box-shadow: 0 0 1px #2196F3;
}

input:checked + .switch-slider:before {
  -webkit-transform: translateX(26px);
  -ms-transform: translateX(26px);
  transform: translateX(26px);
}

/* Rounded sliders */
.switch-slider.round {
  border-radius: 34px;
}

.switch-slider.round:before {
  border-radius: 50%;
} 




@media (max-width: 768px) {
.explorer_menu, .explorer-search {
	float: none;
    clear: both;
}
.explorer-search {
	margin-bottom: 5px;
}

}

@media (max-width: 480px) {
.large-screen {
	display: none;
}

#stats_updated {
	right: 80px;
}


}

.input-group-btn button {
	border-radius: 0;
}

.break-word {
	overflow-wrap: break-word;
	word-wrap: break-word;
	white-space: pre-line;
	-ms-word-break: break-all;
	word-break: break-word;
}

)";

static const std::string white_theme_css =
    R"(body {
  margin-top: 60px;
}

.table > thead > tr > td.success,
.table > tbody > tr > td.success,
.table > tfoot > tr > td.success,
.table > thead > tr > th.success,
.table > tbody > tr > th.success,
.table > tfoot > tr > th.success,
.table > thead > tr.success > td,
.table > tbody > tr.success > td,
.table > tfoot > tr.success > td,
.table > thead > tr.success > th,
.table > tbody > tr.success > th,
.table > tfoot > tr.success > th {
  background-color: #C3F0E0;
}
.table-hover > tbody > tr > td.success:hover,
.table-hover > tbody > tr > th.success:hover,
.table-hover > tbody > tr.success:hover > td,
.table-hover > tbody > tr:hover > .success,
.table-hover > tbody > tr.success:hover > th {
  background-color: #ADDABB;
}
.table > thead > tr > th {
  color: #336A80;
  border-bottom-color: #336A80;
  text-align: center;
  font-size: 1em;
}
.table > tbody > tr > td {
  border-top-color: #c9e0e9;  
  vertical-align: middle;
  font-family: 'Inconsolata', monospace;
  font-size: 1em;
  text-align: center;
  padding: 8px 2px;
}

td .mtx-ago,
td .timeago {
	font-size: 0.9em;
}

#mem_pool_table td {
	text-align: center;
}

#blocks_rows > tr > td:nth-child(2) {
	text-align: left;
}

#blocks_rows > tr > td:nth-child(4) {
	/*font-size: 85%;*/
}

#content {
	margin-bottom: 300px;
	z-index: 1;
	background-color: #FDFDFD;
}

 /* The switch - the box around the slider */
.switch {
  position: relative;
  display: inline-block;
  width: 60px;
  height: 34px;
  margin: 8px 0;
}

/* Hide default HTML checkbox */
.switch input {
  opacity: 0;
  width: 0;
  height: 0;
}

/* The slider */
.switch-slider {
  position: absolute;
  cursor: pointer;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background-color: #178ACC;
  -webkit-transition: .4s;
  transition: .4s;
}

.switch-slider:before {
  font-family: FontAwesome;
  position: absolute;
  content: "\f186";
  font-size: 18px;
  line-height: 26px;
  text-align: center;
  color: #178ACC;
  height: 26px;
  width: 26px;
  left: 4px;
  bottom: 4px;
  background-color: white;
  -webkit-transition: .4s;
  transition: .4s;
}

input:checked + .switch-slider {
  background-color: #2196F3;
}

input:focus + .switch-slider {
  box-shadow: 0 0 1px #2196F3;
}

input:checked + .switch-slider:before {
  -webkit-transform: translateX(26px);
  -ms-transform: translateX(26px);
  transform: translateX(26px);
}

/* Rounded sliders */
.switch-slider.round {
  border-radius: 34px;
}

.switch-slider.round:before {
  border-radius: 50%;
} 




@media (max-width: 768px) {
.explorer_menu, .explorer-search {
	float: none;
    clear: both;
}
.explorer-search {
	margin-bottom: 5px;
}

}

@media (max-width: 480px) {
.large-screen {
	display: none;
}

#stats_updated {
	right: 80px;
}


}

)";

static const std::string style_css =
    R"(
		
@import url("bootstrap.min.css");

#coinName{
    text-transform: capitalize;
}
#coinIcon{
    text-transform: capitalize;
    width: 1.3em;
    height: 1.3em;
    display: inline-block;
    vertical-align: middle;
    color: white;
    border: 2px solid white;
    border-radius: 50%;
    text-align: center;
    margin-top: -4px;
}
body {
    padding-top: 65px;
    padding-bottom: 80px;
    overflow-y: scroll;
}

#loading{
    font-size: 2em;
}
.stats {
    margin-bottom: 10px;
    margin-top: 5px;
    font-size: 1em;
}
.stats:last-child{
    width: auto;
}
.stats > h3 > i{
    font-size: 0.80em;
    width: 21px;
}
.stats > div{
    padding: 5px 0;
    font-size: 0.85em;
}
.stats > div > .fa {
    width: 25px;
}
.stats > div > span:first-of-type{
    font-weight: bold;
}
#stats_updated{
    opacity: 0;
    height: 40px;
    line-height: 40px;
    color: #e8e8e8;
    font-size: 0.9em;
    position: absolute;
    right: 10px;
    top: 0;
}

footer{
    position: relative;
    bottom: 0;
    width: 100%;
    margin: 30px 0;
}

table td {
    font-family: "Lucida Console", Monaco, monospace;
}

.scrollup{
    opacity:0.3;
    position:fixed;
    bottom:50px;
    right:100px;
    display:none;
    text-align: center;
    line-height: 50px;
    font-size: 50px;
    -webkit-transition: color 200ms ease-in-out;
    -moz-transition:    color 200ms ease-in-out;
    -o-transition:      color 200ms ease-in-out;
    -ms-transition: 	color 200ms ease-out;
    display: none;
    overflow: hidden;
}

.btn-default {
    background-color: #2B3E50;
}


#block.hash, #transaction.hash {
    word-break: break-all;
}

.panel-default>.panel-heading .badge {
    background-color: #2B3E50;
}

.form-control {
    background-color: #485563;
    color: #FFF;
}

.chart-wrapper {
    background-color: #384B5C;
}

.navbar .theme-switch {
    background: transparent;
    border: none;
    line-height: 21px;
    padding-top: 9.5px;
    padding-bottom: 9.5px;
}

.nav-pills>li>a {
    padding: 10px 10px;
}

)";

static const std::string dark_bootstrap_min_css =
    R"(
@import url("https://fonts.googleapis.com/css?family=Lato:300,400,700");/*!
* bootswatch v3.3.7
* Homepage: http://bootswatch.com
* Copyright 2012-2017 Thomas Park
* Licensed under MIT
* Based on Bootstrap
*//*!
* Bootstrap v3.3.7 (http://getbootstrap.com)
* Copyright 2011-2016 Twitter, Inc.
* Licensed under MIT (https://github.com/twbs/bootstrap/blob/master/LICENSE)
*//*! normalize.css v3.0.3 | MIT License | github.com/necolas/normalize.css */html{font-family:sans-serif;-ms-text-size-adjust:100%;-webkit-text-size-adjust:100%}body{margin:0}article,aside,details,figcaption,figure,footer,header,hgroup,main,menu,nav,section,summary{display:block}audio,canvas,progress,video{display:inline-block;vertical-align:baseline}audio:not([controls]){display:none;height:0}[hidden],template{display:none}a{background-color:transparent}a:active,a:hover{outline:0}abbr[title]{border-bottom:1px dotted}b,strong{font-weight:bold}dfn{font-style:italic}h1{font-size:2em;margin:0.67em 0}mark{background:#ff0;color:#000}small{font-size:80%}sub,sup{font-size:75%;line-height:0;position:relative;vertical-align:baseline}sup{top:-0.5em}sub{bottom:-0.25em}img{border:0}svg:not(:root){overflow:hidden}figure{margin:1em 40px}hr{-webkit-box-sizing:content-box;-moz-box-sizing:content-box;box-sizing:content-box;height:0}pre{overflow:auto}code,kbd,pre,samp{font-family:monospace, monospace;font-size:1em}button,input,optgroup,select,textarea{color:inherit;font:inherit;margin:0}button{overflow:visible}button,select{text-transform:none}button,html input[type="button"],input[type="reset"],input[type="submit"]{-webkit-appearance:button;cursor:pointer}button[disabled],html input[disabled]{cursor:default}button::-moz-focus-inner,input::-moz-focus-inner{border:0;padding:0}input{line-height:normal}input[type="checkbox"],input[type="radio"]{-webkit-box-sizing:border-box;-moz-box-sizing:border-box;box-sizing:border-box;padding:0}input[type="number"]::-webkit-inner-spin-button,input[type="number"]::-webkit-outer-spin-button{height:auto}input[type="search"]{-webkit-appearance:textfield;-webkit-box-sizing:content-box;-moz-box-sizing:content-box;box-sizing:content-box}input[type="search"]::-webkit-search-cancel-button,input[type="search"]::-webkit-search-decoration{-webkit-appearance:none}fieldset{border:1px solid #c0c0c0;margin:0 2px;padding:0.35em 0.625em 0.75em}legend{border:0;padding:0}textarea{overflow:auto}optgroup{font-weight:bold}table{border-collapse:collapse;border-spacing:0}td,th{padding:0}/*! Source: https://github.com/h5bp/html5-boilerplate/blob/master/src/css/main.css */@media print{*,*:before,*:after{background:transparent !important;color:#000 !important;-webkit-box-shadow:none !important;box-shadow:none !important;text-shadow:none !important}a,a:visited{text-decoration:underline}a[href]:after{content:" "}a[href^="#"]:after,a[href^="javascript:"]:after{content:""}pre,blockquote{border:1px solid #999;page-break-inside:avoid}thead{display:table-header-group}tr,img{page-break-inside:avoid}img{max-width:100% !important}p,h2,h3{orphans:3;widows:3}h2,h3{page-break-after:avoid}.navbar{display:none}.btn>.caret,.dropup>.btn>.caret{border-top-color:#000 !important}.label{border:1px solid #000}.table{border-collapse:collapse !important}.table td,.table th{background-color:#fff !important}.table-bordered th,.table-bordered td{border:1px solid #ddd !important}}@font-face{font-family:'Glyphicons Halflings';src:url('../fonts/glyphicons-halflings-regular.eot');src:url('../fonts/glyphicons-halflings-regular.eot?#iefix') format('embedded-opentype'),url('../fonts/glyphicons-halflings-regular.woff2') format('woff2'),url('../fonts/glyphicons-halflings-regular.woff') format('woff'),url('../fonts/glyphicons-halflings-regular.ttf') format('truetype'),url('../fonts/glyphicons-halflings-regular.svg#glyphicons_halflingsregular') format('svg')}.glyphicon{position:relative;top:1px;display:inline-block;font-family:'Glyphicons Halflings';font-style:normal;font-weight:normal;line-height:1;-webkit-font-smoothing:antialiased;-moz-osx-font-smoothing:grayscale}.glyphicon-asterisk:before{content:"\002a"}.glyphicon-plus:before{content:"\002b"}.glyphicon-euro:before,.glyphicon-eur:before{content:"\20ac"}.glyphicon-minus:before{content:"\2212"}.glyphicon-cloud:before{content:"\2601"}.glyphicon-envelope:before{content:"\2709"}.glyphicon-pencil:before{content:"\270f"}.glyphicon-glass:before{content:"\e001"}.glyphicon-music:before{content:"\e002"}.glyphicon-search:before{content:"\e003"}.glyphicon-heart:before{content:"\e005"}.glyphicon-star:before{content:"\e006"}.glyphicon-star-empty:before{content:"\e007"}.glyphicon-user:before{content:"\e008"}.glyphicon-film:before{content:"\e009"}.glyphicon-th-large:before{content:"\e010"}.glyphicon-th:before{content:"\e011"}.glyphicon-th-list:before{content:"\e012"}.glyphicon-ok:before{content:"\e013"}.glyphicon-remove:before{content:"\e014"}.glyphicon-zoom-in:before{content:"\e015"}.glyphicon-zoom-out:before{content:"\e016"}.glyphicon-off:before{content:"\e017"}.glyphicon-signal:before{content:"\e018"}.glyphicon-cog:before{content:"\e019"}.glyphicon-trash:before{content:"\e020"}.glyphicon-home:before{content:"\e021"}.glyphicon-file:before{content:"\e022"}.glyphicon-time:before{content:"\e023"}.glyphicon-road:before{content:"\e024"}.glyphicon-download-alt:before{content:"\e025"}.glyphicon-download:before{content:"\e026"}.glyphicon-upload:before{content:"\e027"}.glyphicon-inbox:before{content:"\e028"}.glyphicon-play-circle:before{content:"\e029"}.glyphicon-repeat:before{content:"\e030"}.glyphicon-refresh:before{content:"\e031"}.glyphicon-list-alt:before{content:"\e032"}.glyphicon-lock:before{content:"\e033"}.glyphicon-flag:before{content:"\e034"}.glyphicon-headphones:before{content:"\e035"}.glyphicon-volume-off:before{content:"\e036"}.glyphicon-volume-down:before{content:"\e037"}.glyphicon-volume-up:before{content:"\e038"}.glyphicon-qrcode:before{content:"\e039"}.glyphicon-barcode:before{content:"\e040"}.glyphicon-tag:before{content:"\e041"}.glyphicon-tags:before{content:"\e042"}.glyphicon-book:before{content:"\e043"}.glyphicon-bookmark:before{content:"\e044"}.glyphicon-print:before{content:"\e045"}.glyphicon-camera:before{content:"\e046"}.glyphicon-font:before{content:"\e047"}.glyphicon-bold:before{content:"\e048"}.glyphicon-italic:before{content:"\e049"}.glyphicon-text-height:before{content:"\e050"}.glyphicon-text-width:before{content:"\e051"}.glyphicon-align-left:before{content:"\e052"}.glyphicon-align-center:before{content:"\e053"}.glyphicon-align-right:before{content:"\e054"}.glyphicon-align-justify:before{content:"\e055"}.glyphicon-list:before{content:"\e056"}.glyphicon-indent-left:before{content:"\e057"}.glyphicon-indent-right:before{content:"\e058"}.glyphicon-facetime-video:before{content:"\e059"}.glyphicon-picture:before{content:"\e060"}.glyphicon-map-marker:before{content:"\e062"}.glyphicon-adjust:before{content:"\e063"}.glyphicon-tint:before{content:"\e064"}.glyphicon-edit:before{content:"\e065"}.glyphicon-share:before{content:"\e066"}.glyphicon-check:before{content:"\e067"}.glyphicon-move:before{content:"\e068"}.glyphicon-step-backward:before{content:"\e069"}.glyphicon-fast-backward:before{content:"\e070"}.glyphicon-backward:before{content:"\e071"}.glyphicon-play:before{content:"\e072"}.glyphicon-pause:before{content:"\e073"}.glyphicon-stop:before{content:"\e074"}.glyphicon-forward:before{content:"\e075"}.glyphicon-fast-forward:before{content:"\e076"}.glyphicon-step-forward:before{content:"\e077"}.glyphicon-eject:before{content:"\e078"}.glyphicon-chevron-left:before{content:"\e079"}.glyphicon-chevron-right:before{content:"\e080"}.glyphicon-plus-sign:before{content:"\e081"}.glyphicon-minus-sign:before{content:"\e082"}.glyphicon-remove-sign:before{content:"\e083"}.glyphicon-ok-sign:before{content:"\e084"}.glyphicon-question-sign:before{content:"\e085"}.glyphicon-info-sign:before{content:"\e086"}.glyphicon-screenshot:before{content:"\e087"}.glyphicon-remove-circle:before{content:"\e088"}.glyphicon-ok-circle:before{content:"\e089"}.glyphicon-ban-circle:before{content:"\e090"}.glyphicon-arrow-left:before{content:"\e091"}.glyphicon-arrow-right:before{content:"\e092"}.glyphicon-arrow-up:before{content:"\e093"}.glyphicon-arrow-down:before{content:"\e094"}.glyphicon-share-alt:before{content:"\e095"}.glyphicon-resize-full:before{content:"\e096"}.glyphicon-resize-small:before{content:"\e097"}.glyphicon-exclamation-sign:before{content:"\e101"}.glyphicon-gift:before{content:"\e102"}.glyphicon-leaf:before{content:"\e103"}.glyphicon-fire:before{content:"\e104"}.glyphicon-eye-open:before{content:"\e105"}.glyphicon-eye-close:before{content:"\e106"}.glyphicon-warning-sign:before{content:"\e107"}.glyphicon-plane:before{content:"\e108"}.glyphicon-calendar:before{content:"\e109"}.glyphicon-random:before{content:"\e110"}.glyphicon-comment:before{content:"\e111"}.glyphicon-magnet:before{content:"\e112"}.glyphicon-chevron-up:before{content:"\e113"}.glyphicon-chevron-down:before{content:"\e114"}.glyphicon-retweet:before{content:"\e115"}.glyphicon-shopping-cart:before{content:"\e116"}.glyphicon-folder-close:before{content:"\e117"}.glyphicon-folder-open:before{content:"\e118"}.glyphicon-resize-vertical:before{content:"\e119"}.glyphicon-resize-horizontal:before{content:"\e120"}.glyphicon-hdd:before{content:"\e121"}.glyphicon-bullhorn:before{content:"\e122"}.glyphicon-bell:before{content:"\e123"}.glyphicon-certificate:before{content:"\e124"}.glyphicon-thumbs-up:before{content:"\e125"}.glyphicon-thumbs-down:before{content:"\e126"}.glyphicon-hand-right:before{content:"\e127"}.glyphicon-hand-left:before{content:"\e128"}.glyphicon-hand-up:before{content:"\e129"}.glyphicon-hand-down:before{content:"\e130"}.glyphicon-circle-arrow-right:before{content:"\e131"}.glyphicon-circle-arrow-left:before{content:"\e132"}.glyphicon-circle-arrow-up:before{content:"\e133"}.glyphicon-circle-arrow-down:before{content:"\e134"}.glyphicon-globe:before{content:"\e135"}.glyphicon-wrench:before{content:"\e136"}.glyphicon-tasks:before{content:"\e137"}.glyphicon-filter:before{content:"\e138"}.glyphicon-briefcase:before{content:"\e139"}.glyphicon-fullscreen:before{content:"\e140"}.glyphicon-dashboard:before{content:"\e141"}.glyphicon-paperclip:before{content:"\e142"}.glyphicon-heart-empty:before{content:"\e143"}.glyphicon-link:before{content:"\e144"}.glyphicon-phone:before{content:"\e145"}.glyphicon-pushpin:before{content:"\e146"}.glyphicon-usd:before{content:"\e148"}.glyphicon-gbp:before{content:"\e149"}.glyphicon-sort:before{content:"\e150"}.glyphicon-sort-by-alphabet:before{content:"\e151"}.glyphicon-sort-by-alphabet-alt:before{content:"\e152"}.glyphicon-sort-by-order:before{content:"\e153"}.glyphicon-sort-by-order-alt:before{content:"\e154"}.glyphicon-sort-by-attributes:before{content:"\e155"}.glyphicon-sort-by-attributes-alt:before{content:"\e156"}.glyphicon-unchecked:before{content:"\e157"}.glyphicon-expand:before{content:"\e158"}.glyphicon-collapse-down:before{content:"\e159"}.glyphicon-collapse-up:before{content:"\e160"}.glyphicon-log-in:before{content:"\e161"}.glyphicon-flash:before{content:"\e162"}.glyphicon-log-out:before{content:"\e163"}.glyphicon-new-window:before{content:"\e164"}.glyphicon-record:before{content:"\e165"}.glyphicon-save:before{content:"\e166"}.glyphicon-open:before{content:"\e167"}.glyphicon-saved:before{content:"\e168"}.glyphicon-import:before{content:"\e169"}.glyphicon-export:before{content:"\e170"}.glyphicon-send:before{content:"\e171"}.glyphicon-floppy-disk:before{content:"\e172"}.glyphicon-floppy-saved:before{content:"\e173"}.glyphicon-floppy-remove:before{content:"\e174"}.glyphicon-floppy-save:before{content:"\e175"}.glyphicon-floppy-open:before{content:"\e176"}.glyphicon-credit-card:before{content:"\e177"}.glyphicon-transfer:before{content:"\e178"}.glyphicon-cutlery:before{content:"\e179"}.glyphicon-header:before{content:"\e180"}.glyphicon-compressed:before{content:"\e181"}.glyphicon-earphone:before{content:"\e182"}.glyphicon-phone-alt:before{content:"\e183"}.glyphicon-tower:before{content:"\e184"}.glyphicon-stats:before{content:"\e185"}.glyphicon-sd-video:before{content:"\e186"}.glyphicon-hd-video:before{content:"\e187"}.glyphicon-subtitles:before{content:"\e188"}.glyphicon-sound-stereo:before{content:"\e189"}.glyphicon-sound-dolby:before{content:"\e190"}.glyphicon-sound-5-1:before{content:"\e191"}.glyphicon-sound-6-1:before{content:"\e192"}.glyphicon-sound-7-1:before{content:"\e193"}.glyphicon-copyright-mark:before{content:"\e194"}.glyphicon-registration-mark:before{content:"\e195"}.glyphicon-cloud-download:before{content:"\e197"}.glyphicon-cloud-upload:before{content:"\e198"}.glyphicon-tree-conifer:before{content:"\e199"}.glyphicon-tree-deciduous:before{content:"\e200"}.glyphicon-cd:before{content:"\e201"}.glyphicon-save-file:before{content:"\e202"}.glyphicon-open-file:before{content:"\e203"}.glyphicon-level-up:before{content:"\e204"}.glyphicon-copy:before{content:"\e205"}.glyphicon-paste:before{content:"\e206"}.glyphicon-alert:before{content:"\e209"}.glyphicon-equalizer:before{content:"\e210"}.glyphicon-king:before{content:"\e211"}.glyphicon-queen:before{content:"\e212"}.glyphicon-pawn:before{content:"\e213"}.glyphicon-bishop:before{content:"\e214"}.glyphicon-knight:before{content:"\e215"}.glyphicon-baby-formula:before{content:"\e216"}.glyphicon-tent:before{content:"\26fa"}.glyphicon-blackboard:before{content:"\e218"}.glyphicon-bed:before{content:"\e219"}.glyphicon-apple:before{content:"\f8ff"}.glyphicon-erase:before{content:"\e221"}.glyphicon-hourglass:before{content:"\231b"}.glyphicon-lamp:before{content:"\e223"}.glyphicon-duplicate:before{content:"\e224"}.glyphicon-piggy-bank:before{content:"\e225"}.glyphicon-scissors:before{content:"\e226"}.glyphicon-bitcoin:before{content:"\e227"}.glyphicon-btc:before{content:"\e227"}.glyphicon-xbt:before{content:"\e227"}.glyphicon-yen:before{content:"\00a5"}.glyphicon-jpy:before{content:"\00a5"}.glyphicon-ruble:before{content:"\20bd"}.glyphicon-rub:before{content:"\20bd"}.glyphicon-scale:before{content:"\e230"}.glyphicon-ice-lolly:before{content:"\e231"}.glyphicon-ice-lolly-tasted:before{content:"\e232"}.glyphicon-education:before{content:"\e233"}.glyphicon-option-horizontal:before{content:"\e234"}.glyphicon-option-vertical:before{content:"\e235"}.glyphicon-menu-hamburger:before{content:"\e236"}.glyphicon-modal-window:before{content:"\e237"}.glyphicon-oil:before{content:"\e238"}.glyphicon-grain:before{content:"\e239"}.glyphicon-sunglasses:before{content:"\e240"}.glyphicon-text-size:before{content:"\e241"}.glyphicon-text-color:before{content:"\e242"}.glyphicon-text-background:before{content:"\e243"}.glyphicon-object-align-top:before{content:"\e244"}.glyphicon-object-align-bottom:before{content:"\e245"}.glyphicon-object-align-horizontal:before{content:"\e246"}.glyphicon-object-align-left:before{content:"\e247"}.glyphicon-object-align-vertical:before{content:"\e248"}.glyphicon-object-align-right:before{content:"\e249"}.glyphicon-triangle-right:before{content:"\e250"}.glyphicon-triangle-left:before{content:"\e251"}.glyphicon-triangle-bottom:before{content:"\e252"}.glyphicon-triangle-top:before{content:"\e253"}.glyphicon-console:before{content:"\e254"}.glyphicon-superscript:before{content:"\e255"}.glyphicon-subscript:before{content:"\e256"}.glyphicon-menu-left:before{content:"\e257"}.glyphicon-menu-right:before{content:"\e258"}.glyphicon-menu-down:before{content:"\e259"}.glyphicon-menu-up:before{content:"\e260"}*{-webkit-box-sizing:border-box;-moz-box-sizing:border-box;box-sizing:border-box}*:before,*:after{-webkit-box-sizing:border-box;-moz-box-sizing:border-box;box-sizing:border-box}html{font-size:10px;-webkit-tap-highlight-color:rgba(0,0,0,0)}body{font-family:"Lato","Helvetica Neue",Helvetica,Arial,sans-serif;font-size:15px;line-height:1.42857143;color:#ebebeb;background-color:#2b3e50}input,button,select,textarea{font-family:inherit;font-size:inherit;line-height:inherit}a{color:#991A1C;text-decoration:none}a:hover,a:focus{color:#991A1C;text-decoration:underline}a:focus{outline:5px auto -webkit-focus-ring-color;outline-offset:-2px}figure{margin:0}img{vertical-align:middle}.img-responsive,.thumbnail>img,.thumbnail a>img,.carousel-inner>.item>img,.carousel-inner>.item>a>img{display:block;max-width:100%;height:auto}.img-rounded{border-radius:0}.img-thumbnail{padding:4px;line-height:1.42857143;background-color:#2b3e50;border:1px solid #dddddd;border-radius:0;-webkit-transition:all .2s ease-in-out;-o-transition:all .2s ease-in-out;transition:all .2s ease-in-out;display:inline-block;max-width:100%;height:auto}.img-circle{border-radius:50%}hr{margin-top:21px;margin-bottom:21px;border:0;border-top:1px solid #596a7b}.sr-only{position:absolute;width:1px;height:1px;margin:-1px;padding:0;overflow:hidden;clip:rect(0, 0, 0, 0);border:0}.sr-only-focusable:active,.sr-only-focusable:focus{position:static;width:auto;height:auto;margin:0;overflow:visible;clip:auto}[role="button"]{cursor:pointer}h1,h2,h3,h4,h5,h6,.h1,.h2,.h3,.h4,.h5,.h6{font-family:inherit;font-weight:400;line-height:1.1;color:inherit}h1 small,h2 small,h3 small,h4 small,h5 small,h6 small,.h1 small,.h2 small,.h3 small,.h4 small,.h5 small,.h6 small,h1 .small,h2 .small,h3 .small,h4 .small,h5 .small,h6 .small,.h1 .small,.h2 .small,.h3 .small,.h4 .small,.h5 .small,.h6 .small{font-weight:normal;line-height:1;color:#ebebeb}h1,.h1,h2,.h2,h3,.h3{margin-top:21px;margin-bottom:10.5px}h1 small,.h1 small,h2 small,.h2 small,h3 small,.h3 small,h1 .small,.h1 .small,h2 .small,.h2 .small,h3 .small,.h3 .small{font-size:65%}h4,.h4,h5,.h5,h6,.h6{margin-top:10.5px;margin-bottom:10.5px}h4 small,.h4 small,h5 small,.h5 small,h6 small,.h6 small,h4 .small,.h4 .small,h5 .small,.h5 .small,h6 .small,.h6 .small{font-size:75%}h1,.h1{font-size:39px}h2,.h2{font-size:32px}h3,.h3{font-size:26px}h4,.h4{font-size:19px}h5,.h5{font-size:15px}h6,.h6{font-size:13px}p{margin:0 0 10.5px}.lead{margin-bottom:21px;font-size:17px;font-weight:300;line-height:1.4}@media (min-width:768px){.lead{font-size:22.5px}}small,.small{font-size:80%}mark,.mark{background-color:#f0ad4e;padding:.2em}.text-left{text-align:left}.text-right{text-align:right}.text-center{text-align:center}.text-justify{text-align:justify}.text-nowrap{white-space:nowrap}.text-lowercase{text-transform:lowercase}.text-uppercase{text-transform:uppercase}.text-capitalize{text-transform:capitalize}.text-muted{color:#4e5d6c}.text-primary{color:#991A1C}a.text-primary:hover,a.text-primary:focus{color:#b15315}.text-success{color:#ebebeb}a.text-success:hover,a.text-success:focus{color:#d2d2d2}.text-info{color:#ebebeb}a.text-info:hover,a.text-info:focus{color:#d2d2d2}.text-warning{color:#ebebeb}a.text-warning:hover,a.text-warning:focus{color:#d2d2d2}.text-danger{color:#ebebeb}a.text-danger:hover,a.text-danger:focus{color:#d2d2d2}.bg-primary{color:#fff;background-color:#991A1C}a.bg-primary:hover,a.bg-primary:focus{background-color:#b15315}.bg-success{background-color:#5cb85c}a.bg-success:hover,a.bg-success:focus{background-color:#449d44}.bg-info{background-color:#5bc0de}a.bg-info:hover,a.bg-info:focus{background-color:#31b0d5}.bg-warning{background-color:#f0ad4e}a.bg-warning:hover,a.bg-warning:focus{background-color:#ec971f}.bg-danger{background-color:#d9534f}a.bg-danger:hover,a.bg-danger:focus{background-color:#c9302c}.page-header{padding-bottom:9.5px;margin:42px 0 21px;border-bottom:1px solid #ebebeb}ul,ol{margin-top:0;margin-bottom:10.5px}ul ul,ol ul,ul ol,ol ol{margin-bottom:0}.list-unstyled{padding-left:0;list-style:none}.list-inline{padding-left:0;list-style:none;margin-left:-5px}.list-inline>li{display:inline-block;padding-left:5px;padding-right:5px}dl{margin-top:0;margin-bottom:21px}dt,dd{line-height:1.42857143}dt{font-weight:bold}dd{margin-left:0}@media (min-width:768px){.dl-horizontal dt{float:left;width:160px;clear:left;text-align:right;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.dl-horizontal dd{margin-left:180px}}abbr[title],abbr[data-original-title]{cursor:help;border-bottom:1px dotted #4e5d6c}.initialism{font-size:90%;text-transform:uppercase}blockquote{padding:10.5px 21px;margin:0 0 21px;font-size:18.75px;border-left:5px solid #4e5d6c}blockquote p:last-child,blockquote ul:last-child,blockquote ol:last-child{margin-bottom:0}blockquote footer,blockquote small,blockquote .small{display:block;font-size:80%;line-height:1.42857143;color:#ebebeb}blockquote footer:before,blockquote small:before,blockquote .small:before{content:'\2014 \00A0'}.blockquote-reverse,blockquote.pull-right{padding-right:15px;padding-left:0;border-right:5px solid #4e5d6c;border-left:0;text-align:right}.blockquote-reverse footer:before,blockquote.pull-right footer:before,.blockquote-reverse small:before,blockquote.pull-right small:before,.blockquote-reverse .small:before,blockquote.pull-right .small:before{content:''}.blockquote-reverse footer:after,blockquote.pull-right footer:after,.blockquote-reverse small:after,blockquote.pull-right small:after,.blockquote-reverse .small:after,blockquote.pull-right .small:after{content:'\00A0 \2014'}address{margin-bottom:21px;font-style:normal;line-height:1.42857143}code,kbd,pre,samp{font-family:Menlo,Monaco,Consolas,"Courier New",monospace}code{padding:2px 4px;font-size:90%;color:#c7254e;background-color:#f9f2f4;border-radius:0}kbd{padding:2px 4px;font-size:90%;color:#ffffff;background-color:#333333;border-radius:0;-webkit-box-shadow:inset 0 -1px 0 rgba(0,0,0,0.25);box-shadow:inset 0 -1px 0 rgba(0,0,0,0.25)}kbd kbd{padding:0;font-size:100%;font-weight:bold;-webkit-box-shadow:none;box-shadow:none}pre{display:block;padding:10px;margin:0 0 10.5px;font-size:14px;line-height:1.42857143;word-break:break-all;word-wrap:break-word;color:#333333;background-color:#f5f5f5;border:1px solid #cccccc;border-radius:0}pre code{padding:0;font-size:inherit;color:inherit;white-space:pre-wrap;background-color:transparent;border-radius:0}.pre-scrollable{max-height:340px;overflow-y:scroll}.container{margin-right:auto;margin-left:auto;padding-left:15px;padding-right:15px}@media (min-width:768px){.container{width:750px}}@media (min-width:992px){.container{width:970px}}@media (min-width:1200px){.container{width:1170px}}.container-fluid{margin-right:auto;margin-left:auto;padding-left:15px;padding-right:15px}.row{margin-left:-15px;margin-right:-15px}.col-xs-1,.col-sm-1,.col-md-1,.col-lg-1,.col-xs-2,.col-sm-2,.col-md-2,.col-lg-2,.col-xs-3,.col-sm-3,.col-md-3,.col-lg-3,.col-xs-4,.col-sm-4,.col-md-4,.col-lg-4,.col-xs-5,.col-sm-5,.col-md-5,.col-lg-5,.col-xs-6,.col-sm-6,.col-md-6,.col-lg-6,.col-xs-7,.col-sm-7,.col-md-7,.col-lg-7,.col-xs-8,.col-sm-8,.col-md-8,.col-lg-8,.col-xs-9,.col-sm-9,.col-md-9,.col-lg-9,.col-xs-10,.col-sm-10,.col-md-10,.col-lg-10,.col-xs-11,.col-sm-11,.col-md-11,.col-lg-11,.col-xs-12,.col-sm-12,.col-md-12,.col-lg-12{position:relative;min-height:1px;padding-left:15px;padding-right:15px}.col-xs-1,.col-xs-2,.col-xs-3,.col-xs-4,.col-xs-5,.col-xs-6,.col-xs-7,.col-xs-8,.col-xs-9,.col-xs-10,.col-xs-11,.col-xs-12{float:left}.col-xs-12{width:100%}.col-xs-11{width:91.66666667%}.col-xs-10{width:83.33333333%}.col-xs-9{width:75%}.col-xs-8{width:66.66666667%}.col-xs-7{width:58.33333333%}.col-xs-6{width:50%}.col-xs-5{width:41.66666667%}.col-xs-4{width:33.33333333%}.col-xs-3{width:25%}.col-xs-2{width:16.66666667%}.col-xs-1{width:8.33333333%}.col-xs-pull-12{right:100%}.col-xs-pull-11{right:91.66666667%}.col-xs-pull-10{right:83.33333333%}.col-xs-pull-9{right:75%}.col-xs-pull-8{right:66.66666667%}.col-xs-pull-7{right:58.33333333%}.col-xs-pull-6{right:50%}.col-xs-pull-5{right:41.66666667%}.col-xs-pull-4{right:33.33333333%}.col-xs-pull-3{right:25%}.col-xs-pull-2{right:16.66666667%}.col-xs-pull-1{right:8.33333333%}.col-xs-pull-0{right:auto}.col-xs-push-12{left:100%}.col-xs-push-11{left:91.66666667%}.col-xs-push-10{left:83.33333333%}.col-xs-push-9{left:75%}.col-xs-push-8{left:66.66666667%}.col-xs-push-7{left:58.33333333%}.col-xs-push-6{left:50%}.col-xs-push-5{left:41.66666667%}.col-xs-push-4{left:33.33333333%}.col-xs-push-3{left:25%}.col-xs-push-2{left:16.66666667%}.col-xs-push-1{left:8.33333333%}.col-xs-push-0{left:auto}.col-xs-offset-12{margin-left:100%}.col-xs-offset-11{margin-left:91.66666667%}.col-xs-offset-10{margin-left:83.33333333%}.col-xs-offset-9{margin-left:75%}.col-xs-offset-8{margin-left:66.66666667%}.col-xs-offset-7{margin-left:58.33333333%}.col-xs-offset-6{margin-left:50%}.col-xs-offset-5{margin-left:41.66666667%}.col-xs-offset-4{margin-left:33.33333333%}.col-xs-offset-3{margin-left:25%}.col-xs-offset-2{margin-left:16.66666667%}.col-xs-offset-1{margin-left:8.33333333%}.col-xs-offset-0{margin-left:0%}@media (min-width:768px){.col-sm-1,.col-sm-2,.col-sm-3,.col-sm-4,.col-sm-5,.col-sm-6,.col-sm-7,.col-sm-8,.col-sm-9,.col-sm-10,.col-sm-11,.col-sm-12{float:left}.col-sm-12{width:100%}.col-sm-11{width:91.66666667%}.col-sm-10{width:83.33333333%}.col-sm-9{width:75%}.col-sm-8{width:66.66666667%}.col-sm-7{width:58.33333333%}.col-sm-6{width:50%}.col-sm-5{width:41.66666667%}.col-sm-4{width:33.33333333%}.col-sm-3{width:25%}.col-sm-2{width:16.66666667%}.col-sm-1{width:8.33333333%}.col-sm-pull-12{right:100%}.col-sm-pull-11{right:91.66666667%}.col-sm-pull-10{right:83.33333333%}.col-sm-pull-9{right:75%}.col-sm-pull-8{right:66.66666667%}.col-sm-pull-7{right:58.33333333%}.col-sm-pull-6{right:50%}.col-sm-pull-5{right:41.66666667%}.col-sm-pull-4{right:33.33333333%}.col-sm-pull-3{right:25%}.col-sm-pull-2{right:16.66666667%}.col-sm-pull-1{right:8.33333333%}.col-sm-pull-0{right:auto}.col-sm-push-12{left:100%}.col-sm-push-11{left:91.66666667%}.col-sm-push-10{left:83.33333333%}.col-sm-push-9{left:75%}.col-sm-push-8{left:66.66666667%}.col-sm-push-7{left:58.33333333%}.col-sm-push-6{left:50%}.col-sm-push-5{left:41.66666667%}.col-sm-push-4{left:33.33333333%}.col-sm-push-3{left:25%}.col-sm-push-2{left:16.66666667%}.col-sm-push-1{left:8.33333333%}.col-sm-push-0{left:auto}.col-sm-offset-12{margin-left:100%}.col-sm-offset-11{margin-left:91.66666667%}.col-sm-offset-10{margin-left:83.33333333%}.col-sm-offset-9{margin-left:75%}.col-sm-offset-8{margin-left:66.66666667%}.col-sm-offset-7{margin-left:58.33333333%}.col-sm-offset-6{margin-left:50%}.col-sm-offset-5{margin-left:41.66666667%}.col-sm-offset-4{margin-left:33.33333333%}.col-sm-offset-3{margin-left:25%}.col-sm-offset-2{margin-left:16.66666667%}.col-sm-offset-1{margin-left:8.33333333%}.col-sm-offset-0{margin-left:0%}}@media (min-width:992px){.col-md-1,.col-md-2,.col-md-3,.col-md-4,.col-md-5,.col-md-6,.col-md-7,.col-md-8,.col-md-9,.col-md-10,.col-md-11,.col-md-12{float:left}.col-md-12{width:100%}.col-md-11{width:91.66666667%}.col-md-10{width:83.33333333%}.col-md-9{width:75%}.col-md-8{width:66.66666667%}.col-md-7{width:58.33333333%}.col-md-6{width:50%}.col-md-5{width:41.66666667%}.col-md-4{width:33.33333333%}.col-md-3{width:25%}.col-md-2{width:16.66666667%}.col-md-1{width:8.33333333%}.col-md-pull-12{right:100%}.col-md-pull-11{right:91.66666667%}.col-md-pull-10{right:83.33333333%}.col-md-pull-9{right:75%}.col-md-pull-8{right:66.66666667%}.col-md-pull-7{right:58.33333333%}.col-md-pull-6{right:50%}.col-md-pull-5{right:41.66666667%}.col-md-pull-4{right:33.33333333%}.col-md-pull-3{right:25%}.col-md-pull-2{right:16.66666667%}.col-md-pull-1{right:8.33333333%}.col-md-pull-0{right:auto}.col-md-push-12{left:100%}.col-md-push-11{left:91.66666667%}.col-md-push-10{left:83.33333333%}.col-md-push-9{left:75%}.col-md-push-8{left:66.66666667%}.col-md-push-7{left:58.33333333%}.col-md-push-6{left:50%}.col-md-push-5{left:41.66666667%}.col-md-push-4{left:33.33333333%}.col-md-push-3{left:25%}.col-md-push-2{left:16.66666667%}.col-md-push-1{left:8.33333333%}.col-md-push-0{left:auto}.col-md-offset-12{margin-left:100%}.col-md-offset-11{margin-left:91.66666667%}.col-md-offset-10{margin-left:83.33333333%}.col-md-offset-9{margin-left:75%}.col-md-offset-8{margin-left:66.66666667%}.col-md-offset-7{margin-left:58.33333333%}.col-md-offset-6{margin-left:50%}.col-md-offset-5{margin-left:41.66666667%}.col-md-offset-4{margin-left:33.33333333%}.col-md-offset-3{margin-left:25%}.col-md-offset-2{margin-left:16.66666667%}.col-md-offset-1{margin-left:8.33333333%}.col-md-offset-0{margin-left:0%}}@media (min-width:1200px){.col-lg-1,.col-lg-2,.col-lg-3,.col-lg-4,.col-lg-5,.col-lg-6,.col-lg-7,.col-lg-8,.col-lg-9,.col-lg-10,.col-lg-11,.col-lg-12{float:left}.col-lg-12{width:100%}.col-lg-11{width:91.66666667%}.col-lg-10{width:83.33333333%}.col-lg-9{width:75%}.col-lg-8{width:66.66666667%}.col-lg-7{width:58.33333333%}.col-lg-6{width:50%}.col-lg-5{width:41.66666667%}.col-lg-4{width:33.33333333%}.col-lg-3{width:25%}.col-lg-2{width:16.66666667%}.col-lg-1{width:8.33333333%}.col-lg-pull-12{right:100%}.col-lg-pull-11{right:91.66666667%}.col-lg-pull-10{right:83.33333333%}.col-lg-pull-9{right:75%}.col-lg-pull-8{right:66.66666667%}.col-lg-pull-7{right:58.33333333%}.col-lg-pull-6{right:50%}.col-lg-pull-5{right:41.66666667%}.col-lg-pull-4{right:33.33333333%}.col-lg-pull-3{right:25%}.col-lg-pull-2{right:16.66666667%}.col-lg-pull-1{right:8.33333333%}.col-lg-pull-0{right:auto}.col-lg-push-12{left:100%}.col-lg-push-11{left:91.66666667%}.col-lg-push-10{left:83.33333333%}.col-lg-push-9{left:75%}.col-lg-push-8{left:66.66666667%}.col-lg-push-7{left:58.33333333%}.col-lg-push-6{left:50%}.col-lg-push-5{left:41.66666667%}.col-lg-push-4{left:33.33333333%}.col-lg-push-3{left:25%}.col-lg-push-2{left:16.66666667%}.col-lg-push-1{left:8.33333333%}.col-lg-push-0{left:auto}.col-lg-offset-12{margin-left:100%}.col-lg-offset-11{margin-left:91.66666667%}.col-lg-offset-10{margin-left:83.33333333%}.col-lg-offset-9{margin-left:75%}.col-lg-offset-8{margin-left:66.66666667%}.col-lg-offset-7{margin-left:58.33333333%}.col-lg-offset-6{margin-left:50%}.col-lg-offset-5{margin-left:41.66666667%}.col-lg-offset-4{margin-left:33.33333333%}.col-lg-offset-3{margin-left:25%}.col-lg-offset-2{margin-left:16.66666667%}.col-lg-offset-1{margin-left:8.33333333%}.col-lg-offset-0{margin-left:0%}}table{background-color:transparent}caption{padding-top:6px;padding-bottom:6px;color:#4e5d6c;text-align:left}th{text-align:left}.table{width:100%;max-width:100%;margin-bottom:21px}.table>thead>tr>th,.table>tbody>tr>th,.table>tfoot>tr>th,.table>thead>tr>td,.table>tbody>tr>td,.table>tfoot>tr>td{padding:6px;line-height:1.42857143;vertical-align:top;border-top:1px solid #4e5d6c}.table>thead>tr>th{vertical-align:bottom;border-bottom:2px solid #4e5d6c}.table>caption+thead>tr:first-child>th,.table>colgroup+thead>tr:first-child>th,.table>thead:first-child>tr:first-child>th,.table>caption+thead>tr:first-child>td,.table>colgroup+thead>tr:first-child>td,.table>thead:first-child>tr:first-child>td{border-top:0}.table>tbody+tbody{border-top:2px solid #4e5d6c}.table .table{background-color:#2b3e50}.table-condensed>thead>tr>th,.table-condensed>tbody>tr>th,.table-condensed>tfoot>tr>th,.table-condensed>thead>tr>td,.table-condensed>tbody>tr>td,.table-condensed>tfoot>tr>td{padding:3px}.table-bordered{border:1px solid #4e5d6c}.table-bordered>thead>tr>th,.table-bordered>tbody>tr>th,.table-bordered>tfoot>tr>th,.table-bordered>thead>tr>td,.table-bordered>tbody>tr>td,.table-bordered>tfoot>tr>td{border:1px solid #4e5d6c}.table-bordered>thead>tr>th,.table-bordered>thead>tr>td{border-bottom-width:2px}.table-striped>tbody>tr:nth-of-type(odd){background-color:#4e5d6c}.table-hover>tbody>tr:hover{background-color:#485563}table col[class*="col-"]{position:static;float:none;display:table-column}table td[class*="col-"],table th[class*="col-"]{position:static;float:none;display:table-cell}.table>thead>tr>td.active,.table>tbody>tr>td.active,.table>tfoot>tr>td.active,.table>thead>tr>th.active,.table>tbody>tr>th.active,.table>tfoot>tr>th.active,.table>thead>tr.active>td,.table>tbody>tr.active>td,.table>tfoot>tr.active>td,.table>thead>tr.active>th,.table>tbody>tr.active>th,.table>tfoot>tr.active>th{background-color:#485563}.table-hover>tbody>tr>td.active:hover,.table-hover>tbody>tr>th.active:hover,.table-hover>tbody>tr.active:hover>td,.table-hover>tbody>tr:hover>.active,.table-hover>tbody>tr.active:hover>th{background-color:#3d4954}.table>thead>tr>td.success,.table>tbody>tr>td.success,.table>tfoot>tr>td.success,.table>thead>tr>th.success,.table>tbody>tr>th.success,.table>tfoot>tr>th.success,.table>thead>tr.success>td,.table>tbody>tr.success>td,.table>tfoot>tr.success>td,.table>thead>tr.success>th,.table>tbody>tr.success>th,.table>tfoot>tr.success>th{background-color:#5cb85c}.table-hover>tbody>tr>td.success:hover,.table-hover>tbody>tr>th.success:hover,.table-hover>tbody>tr.success:hover>td,.table-hover>tbody>tr:hover>.success,.table-hover>tbody>tr.success:hover>th{background-color:#4cae4c}.table>thead>tr>td.info,.table>tbody>tr>td.info,.table>tfoot>tr>td.info,.table>thead>tr>th.info,.table>tbody>tr>th.info,.table>tfoot>tr>th.info,.table>thead>tr.info>td,.table>tbody>tr.info>td,.table>tfoot>tr.info>td,.table>thead>tr.info>th,.table>tbody>tr.info>th,.table>tfoot>tr.info>th{background-color:#5bc0de}.table-hover>tbody>tr>td.info:hover,.table-hover>tbody>tr>th.info:hover,.table-hover>tbody>tr.info:hover>td,.table-hover>tbody>tr:hover>.info,.table-hover>tbody>tr.info:hover>th{background-color:#46b8da}.table>thead>tr>td.warning,.table>tbody>tr>td.warning,.table>tfoot>tr>td.warning,.table>thead>tr>th.warning,.table>tbody>tr>th.warning,.table>tfoot>tr>th.warning,.table>thead>tr.warning>td,.table>tbody>tr.warning>td,.table>tfoot>tr.warning>td,.table>thead>tr.warning>th,.table>tbody>tr.warning>th,.table>tfoot>tr.warning>th{background-color:#f0ad4e}.table-hover>tbody>tr>td.warning:hover,.table-hover>tbody>tr>th.warning:hover,.table-hover>tbody>tr.warning:hover>td,.table-hover>tbody>tr:hover>.warning,.table-hover>tbody>tr.warning:hover>th{background-color:#eea236}.table>thead>tr>td.danger,.table>tbody>tr>td.danger,.table>tfoot>tr>td.danger,.table>thead>tr>th.danger,.table>tbody>tr>th.danger,.table>tfoot>tr>th.danger,.table>thead>tr.danger>td,.table>tbody>tr.danger>td,.table>tfoot>tr.danger>td,.table>thead>tr.danger>th,.table>tbody>tr.danger>th,.table>tfoot>tr.danger>th{background-color:#d9534f}.table-hover>tbody>tr>td.danger:hover,.table-hover>tbody>tr>th.danger:hover,.table-hover>tbody>tr.danger:hover>td,.table-hover>tbody>tr:hover>.danger,.table-hover>tbody>tr.danger:hover>th{background-color:#d43f3a}.table-responsive{overflow-x:auto;min-height:0.01%}@media screen and (max-width:767px){.table-responsive{width:100%;margin-bottom:15.75px;overflow-y:hidden;-ms-overflow-style:-ms-autohiding-scrollbar;border:1px solid #4e5d6c}.table-responsive>.table{margin-bottom:0}.table-responsive>.table>thead>tr>th,.table-responsive>.table>tbody>tr>th,.table-responsive>.table>tfoot>tr>th,.table-responsive>.table>thead>tr>td,.table-responsive>.table>tbody>tr>td,.table-responsive>.table>tfoot>tr>td{white-space:nowrap}.table-responsive>.table-bordered{border:0}.table-responsive>.table-bordered>thead>tr>th:first-child,.table-responsive>.table-bordered>tbody>tr>th:first-child,.table-responsive>.table-bordered>tfoot>tr>th:first-child,.table-responsive>.table-bordered>thead>tr>td:first-child,.table-responsive>.table-bordered>tbody>tr>td:first-child,.table-responsive>.table-bordered>tfoot>tr>td:first-child{border-left:0}.table-responsive>.table-bordered>thead>tr>th:last-child,.table-responsive>.table-bordered>tbody>tr>th:last-child,.table-responsive>.table-bordered>tfoot>tr>th:last-child,.table-responsive>.table-bordered>thead>tr>td:last-child,.table-responsive>.table-bordered>tbody>tr>td:last-child,.table-responsive>.table-bordered>tfoot>tr>td:last-child{border-right:0}.table-responsive>.table-bordered>tbody>tr:last-child>th,.table-responsive>.table-bordered>tfoot>tr:last-child>th,.table-responsive>.table-bordered>tbody>tr:last-child>td,.table-responsive>.table-bordered>tfoot>tr:last-child>td{border-bottom:0}}fieldset{padding:0;margin:0;border:0;min-width:0}legend{display:block;width:100%;padding:0;margin-bottom:21px;font-size:22.5px;line-height:inherit;color:#ebebeb;border:0;border-bottom:1px solid #4e5d6c}label{display:inline-block;max-width:100%;margin-bottom:5px;font-weight:bold}input[type="search"]{-webkit-box-sizing:border-box;-moz-box-sizing:border-box;box-sizing:border-box}input[type="radio"],input[type="checkbox"]{margin:4px 0 0;margin-top:1px \9;line-height:normal}input[type="file"]{display:block}input[type="range"]{display:block;width:100%}select[multiple],select[size]{height:auto}input[type="file"]:focus,input[type="radio"]:focus,input[type="checkbox"]:focus{outline:5px auto -webkit-focus-ring-color;outline-offset:-2px}output{display:block;padding-top:9px;font-size:15px;line-height:1.42857143;color:#2b3e50}.form-control{display:block;width:100%;height:39px;padding:8px 16px;font-size:15px;line-height:1.42857143;color:#2b3e50;background-color:#ffffff;background-image:none;border:1px solid transparent;border-radius:0;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.075);box-shadow:inset 0 1px 1px rgba(0,0,0,0.075);-webkit-transition:border-color ease-in-out .15s,-webkit-box-shadow ease-in-out .15s;-o-transition:border-color ease-in-out .15s,box-shadow ease-in-out .15s;transition:border-color ease-in-out .15s,box-shadow ease-in-out .15s}.form-control:focus{border-color:transparent;outline:0;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.075),0 0 8px rgba(0,0,0,0.6);box-shadow:inset 0 1px 1px rgba(0,0,0,0.075),0 0 8px rgba(0,0,0,0.6)}.form-control::-moz-placeholder{color:#cccccc;opacity:1}.form-control:-ms-input-placeholder{color:#cccccc}.form-control::-webkit-input-placeholder{color:#cccccc}.form-control::-ms-expand{border:0;background-color:transparent}.form-control[disabled],.form-control[readonly],fieldset[disabled] .form-control{background-color:#ebebeb;opacity:1}.form-control[disabled],fieldset[disabled] .form-control{cursor:not-allowed}textarea.form-control{height:auto}input[type="search"]{-webkit-appearance:none}@media screen and (-webkit-min-device-pixel-ratio:0){input[type="date"].form-control,input[type="time"].form-control,input[type="datetime-local"].form-control,input[type="month"].form-control{line-height:39px}input[type="date"].input-sm,input[type="time"].input-sm,input[type="datetime-local"].input-sm,input[type="month"].input-sm,.input-group-sm input[type="date"],.input-group-sm input[type="time"],.input-group-sm input[type="datetime-local"],.input-group-sm input[type="month"]{line-height:30px}input[type="date"].input-lg,input[type="time"].input-lg,input[type="datetime-local"].input-lg,input[type="month"].input-lg,.input-group-lg input[type="date"],.input-group-lg input[type="time"],.input-group-lg input[type="datetime-local"],.input-group-lg input[type="month"]{line-height:52px}}.form-group{margin-bottom:15px}.radio,.checkbox{position:relative;display:block;margin-top:10px;margin-bottom:10px}.radio label,.checkbox label{min-height:21px;padding-left:20px;margin-bottom:0;font-weight:normal;cursor:pointer}.radio input[type="radio"],.radio-inline input[type="radio"],.checkbox input[type="checkbox"],.checkbox-inline input[type="checkbox"]{position:absolute;margin-left:-20px;margin-top:4px \9}.radio+.radio,.checkbox+.checkbox{margin-top:-5px}.radio-inline,.checkbox-inline{position:relative;display:inline-block;padding-left:20px;margin-bottom:0;vertical-align:middle;font-weight:normal;cursor:pointer}.radio-inline+.radio-inline,.checkbox-inline+.checkbox-inline{margin-top:0;margin-left:10px}input[type="radio"][disabled],input[type="checkbox"][disabled],input[type="radio"].disabled,input[type="checkbox"].disabled,fieldset[disabled] input[type="radio"],fieldset[disabled] input[type="checkbox"]{cursor:not-allowed}.radio-inline.disabled,.checkbox-inline.disabled,fieldset[disabled] .radio-inline,fieldset[disabled] .checkbox-inline{cursor:not-allowed}.radio.disabled label,.checkbox.disabled label,fieldset[disabled] .radio label,fieldset[disabled] .checkbox label{cursor:not-allowed}.form-control-static{padding-top:9px;padding-bottom:9px;margin-bottom:0;min-height:36px}.form-control-static.input-lg,.form-control-static.input-sm{padding-left:0;padding-right:0}.input-sm{height:30px;padding:5px 10px;font-size:12px;line-height:1.5;border-radius:0}select.input-sm{height:30px;line-height:30px}textarea.input-sm,select[multiple].input-sm{height:auto}.form-group-sm .form-control{height:30px;padding:5px 10px;font-size:12px;line-height:1.5;border-radius:0}.form-group-sm select.form-control{height:30px;line-height:30px}.form-group-sm textarea.form-control,.form-group-sm select[multiple].form-control{height:auto}.form-group-sm .form-control-static{height:30px;min-height:33px;padding:6px 10px;font-size:12px;line-height:1.5}.input-lg{height:52px;padding:12px 24px;font-size:19px;line-height:1.3333333;border-radius:0}select.input-lg{height:52px;line-height:52px}textarea.input-lg,select[multiple].input-lg{height:auto}.form-group-lg .form-control{height:52px;padding:12px 24px;font-size:19px;line-height:1.3333333;border-radius:0}.form-group-lg select.form-control{height:52px;line-height:52px}.form-group-lg textarea.form-control,.form-group-lg select[multiple].form-control{height:auto}.form-group-lg .form-control-static{height:52px;min-height:40px;padding:13px 24px;font-size:19px;line-height:1.3333333}.has-feedback{position:relative}.has-feedback .form-control{padding-right:48.75px}.form-control-feedback{position:absolute;top:0;right:0;z-index:2;display:block;width:39px;height:39px;line-height:39px;text-align:center;pointer-events:none}.input-lg+.form-control-feedback,.input-group-lg+.form-control-feedback,.form-group-lg .form-control+.form-control-feedback{width:52px;height:52px;line-height:52px}.input-sm+.form-control-feedback,.input-group-sm+.form-control-feedback,.form-group-sm .form-control+.form-control-feedback{width:30px;height:30px;line-height:30px}.has-success .help-block,.has-success .control-label,.has-success .radio,.has-success .checkbox,.has-success .radio-inline,.has-success .checkbox-inline,.has-success.radio label,.has-success.checkbox label,.has-success.radio-inline label,.has-success.checkbox-inline label{color:#ebebeb}.has-success .form-control{border-color:#ebebeb;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.075);box-shadow:inset 0 1px 1px rgba(0,0,0,0.075)}.has-success .form-control:focus{border-color:#d2d2d2;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.075),0 0 6px #fff;box-shadow:inset 0 1px 1px rgba(0,0,0,0.075),0 0 6px #fff}.has-success .input-group-addon{color:#ebebeb;border-color:#ebebeb;background-color:#5cb85c}.has-success .form-control-feedback{color:#ebebeb}.has-warning .help-block,.has-warning .control-label,.has-warning .radio,.has-warning .checkbox,.has-warning .radio-inline,.has-warning .checkbox-inline,.has-warning.radio label,.has-warning.checkbox label,.has-warning.radio-inline label,.has-warning.checkbox-inline label{color:#ebebeb}.has-warning .form-control{border-color:#ebebeb;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.075);box-shadow:inset 0 1px 1px rgba(0,0,0,0.075)}.has-warning .form-control:focus{border-color:#d2d2d2;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.075),0 0 6px #fff;box-shadow:inset 0 1px 1px rgba(0,0,0,0.075),0 0 6px #fff}.has-warning .input-group-addon{color:#ebebeb;border-color:#ebebeb;background-color:#f0ad4e}.has-warning .form-control-feedback{color:#ebebeb}.has-error .help-block,.has-error .control-label,.has-error .radio,.has-error .checkbox,.has-error .radio-inline,.has-error .checkbox-inline,.has-error.radio label,.has-error.checkbox label,.has-error.radio-inline label,.has-error.checkbox-inline label{color:#ebebeb}.has-error .form-control{border-color:#ebebeb;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.075);box-shadow:inset 0 1px 1px rgba(0,0,0,0.075)}.has-error .form-control:focus{border-color:#d2d2d2;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.075),0 0 6px #fff;box-shadow:inset 0 1px 1px rgba(0,0,0,0.075),0 0 6px #fff}.has-error .input-group-addon{color:#ebebeb;border-color:#ebebeb;background-color:#d9534f}.has-error .form-control-feedback{color:#ebebeb}.has-feedback label~.form-control-feedback{top:26px}.has-feedback label.sr-only~.form-control-feedback{top:0}.help-block{display:block;margin-top:5px;margin-bottom:10px;color:#ffffff}@media (min-width:768px){.form-inline .form-group{display:inline-block;margin-bottom:0;vertical-align:middle}.form-inline .form-control{display:inline-block;width:auto;vertical-align:middle}.form-inline .form-control-static{display:inline-block}.form-inline .input-group{display:inline-table;vertical-align:middle}.form-inline .input-group .input-group-addon,.form-inline .input-group .input-group-btn,.form-inline .input-group .form-control{width:auto}.form-inline .input-group>.form-control{width:100%}.form-inline .control-label{margin-bottom:0;vertical-align:middle}.form-inline .radio,.form-inline .checkbox{display:inline-block;margin-top:0;margin-bottom:0;vertical-align:middle}.form-inline .radio label,.form-inline .checkbox label{padding-left:0}.form-inline .radio input[type="radio"],.form-inline .checkbox input[type="checkbox"]{position:relative;margin-left:0}.form-inline .has-feedback .form-control-feedback{top:0}}.form-horizontal .radio,.form-horizontal .checkbox,.form-horizontal .radio-inline,.form-horizontal .checkbox-inline{margin-top:0;margin-bottom:0;padding-top:9px}.form-horizontal .radio,.form-horizontal .checkbox{min-height:30px}.form-horizontal .form-group{margin-left:-15px;margin-right:-15px}@media (min-width:768px){.form-horizontal .control-label{text-align:right;margin-bottom:0;padding-top:9px}}.form-horizontal .has-feedback .form-control-feedback{right:15px}@media (min-width:768px){.form-horizontal .form-group-lg .control-label{padding-top:13px;font-size:19px}}@media (min-width:768px){.form-horizontal .form-group-sm .control-label{padding-top:6px;font-size:12px}}.btn{display:inline-block;margin-bottom:0;font-weight:normal;text-align:center;vertical-align:middle;-ms-touch-action:manipulation;touch-action:manipulation;cursor:pointer;background-image:none;border:1px solid transparent;white-space:nowrap;padding:8px 16px;font-size:15px;line-height:1.42857143;border-radius:0;-webkit-user-select:none;-moz-user-select:none;-ms-user-select:none;user-select:none}.btn:focus,.btn:active:focus,.btn.active:focus,.btn.focus,.btn:active.focus,.btn.active.focus{outline:5px auto -webkit-focus-ring-color;outline-offset:-2px}.btn:hover,.btn:focus,.btn.focus{color:#ffffff;text-decoration:none}.btn:active,.btn.active{outline:0;background-image:none;-webkit-box-shadow:inset 0 3px 5px rgba(0,0,0,0.125);box-shadow:inset 0 3px 5px rgba(0,0,0,0.125)}.btn.disabled,.btn[disabled],fieldset[disabled] .btn{cursor:not-allowed;opacity:0.65;filter:alpha(opacity=65);-webkit-box-shadow:none;box-shadow:none}a.btn.disabled,fieldset[disabled] a.btn{pointer-events:none}.btn-default{color:#ffffff;background-color:#4e5d6c;border-color:transparent}.btn-default:focus,.btn-default.focus{color:#ffffff;background-color:#39444e;border-color:rgba(0,0,0,0)}.btn-default:hover{color:#ffffff;background-color:#39444e;border-color:rgba(0,0,0,0)}.btn-default:active,.btn-default.active,.open>.dropdown-toggle.btn-default{color:#ffffff;background-color:#39444e;border-color:rgba(0,0,0,0)}.btn-default:active:hover,.btn-default.active:hover,.open>.dropdown-toggle.btn-default:hover,.btn-default:active:focus,.btn-default.active:focus,.open>.dropdown-toggle.btn-default:focus,.btn-default:active.focus,.btn-default.active.focus,.open>.dropdown-toggle.btn-default.focus{color:#ffffff;background-color:#2a323a;border-color:rgba(0,0,0,0)}.btn-default:active,.btn-default.active,.open>.dropdown-toggle.btn-default{background-image:none}.btn-default.disabled:hover,.btn-default[disabled]:hover,fieldset[disabled] .btn-default:hover,.btn-default.disabled:focus,.btn-default[disabled]:focus,fieldset[disabled] .btn-default:focus,.btn-default.disabled.focus,.btn-default[disabled].focus,fieldset[disabled] .btn-default.focus{background-color:#4e5d6c;border-color:transparent}.btn-default .badge{color:#4e5d6c;background-color:#ffffff}.btn-primary{color:#ffffff;background-color:#991A1C;border-color:transparent}.btn-primary:focus,.btn-primary.focus{color:#ffffff;background-color:#b15315;border-color:rgba(0,0,0,0)}.btn-primary:hover{color:#ffffff;background-color:#b15315;border-color:rgba(0,0,0,0)}.btn-primary:active,.btn-primary.active,.open>.dropdown-toggle.btn-primary{color:#ffffff;background-color:#b15315;border-color:rgba(0,0,0,0)}.btn-primary:active:hover,.btn-primary.active:hover,.open>.dropdown-toggle.btn-primary:hover,.btn-primary:active:focus,.btn-primary.active:focus,.open>.dropdown-toggle.btn-primary:focus,.btn-primary:active.focus,.btn-primary.active.focus,.open>.dropdown-toggle.btn-primary.focus{color:#ffffff;background-color:#914411;border-color:rgba(0,0,0,0)}.btn-primary:active,.btn-primary.active,.open>.dropdown-toggle.btn-primary{background-image:none}.btn-primary.disabled:hover,.btn-primary[disabled]:hover,fieldset[disabled] .btn-primary:hover,.btn-primary.disabled:focus,.btn-primary[disabled]:focus,fieldset[disabled] .btn-primary:focus,.btn-primary.disabled.focus,.btn-primary[disabled].focus,fieldset[disabled] .btn-primary.focus{background-color:#991A1C;border-color:transparent}.btn-primary .badge{color:#991A1C;background-color:#ffffff}.btn-success{color:#ffffff;background-color:#5cb85c;border-color:transparent}.btn-success:focus,.btn-success.focus{color:#ffffff;background-color:#449d44;border-color:rgba(0,0,0,0)}.btn-success:hover{color:#ffffff;background-color:#449d44;border-color:rgba(0,0,0,0)}.btn-success:active,.btn-success.active,.open>.dropdown-toggle.btn-success{color:#ffffff;background-color:#449d44;border-color:rgba(0,0,0,0)}.btn-success:active:hover,.btn-success.active:hover,.open>.dropdown-toggle.btn-success:hover,.btn-success:active:focus,.btn-success.active:focus,.open>.dropdown-toggle.btn-success:focus,.btn-success:active.focus,.btn-success.active.focus,.open>.dropdown-toggle.btn-success.focus{color:#ffffff;background-color:#398439;border-color:rgba(0,0,0,0)}.btn-success:active,.btn-success.active,.open>.dropdown-toggle.btn-success{background-image:none}.btn-success.disabled:hover,.btn-success[disabled]:hover,fieldset[disabled] .btn-success:hover,.btn-success.disabled:focus,.btn-success[disabled]:focus,fieldset[disabled] .btn-success:focus,.btn-success.disabled.focus,.btn-success[disabled].focus,fieldset[disabled] .btn-success.focus{background-color:#5cb85c;border-color:transparent}.btn-success .badge{color:#5cb85c;background-color:#ffffff}.btn-info{color:#ffffff;background-color:#5bc0de;border-color:transparent}.btn-info:focus,.btn-info.focus{color:#ffffff;background-color:#31b0d5;border-color:rgba(0,0,0,0)}.btn-info:hover{color:#ffffff;background-color:#31b0d5;border-color:rgba(0,0,0,0)}.btn-info:active,.btn-info.active,.open>.dropdown-toggle.btn-info{color:#ffffff;background-color:#31b0d5;border-color:rgba(0,0,0,0)}.btn-info:active:hover,.btn-info.active:hover,.open>.dropdown-toggle.btn-info:hover,.btn-info:active:focus,.btn-info.active:focus,.open>.dropdown-toggle.btn-info:focus,.btn-info:active.focus,.btn-info.active.focus,.open>.dropdown-toggle.btn-info.focus{color:#ffffff;background-color:#269abc;border-color:rgba(0,0,0,0)}.btn-info:active,.btn-info.active,.open>.dropdown-toggle.btn-info{background-image:none}.btn-info.disabled:hover,.btn-info[disabled]:hover,fieldset[disabled] .btn-info:hover,.btn-info.disabled:focus,.btn-info[disabled]:focus,fieldset[disabled] .btn-info:focus,.btn-info.disabled.focus,.btn-info[disabled].focus,fieldset[disabled] .btn-info.focus{background-color:#5bc0de;border-color:transparent}.btn-info .badge{color:#5bc0de;background-color:#ffffff}.btn-warning{color:#ffffff;background-color:#f0ad4e;border-color:transparent}.btn-warning:focus,.btn-warning.focus{color:#ffffff;background-color:#ec971f;border-color:rgba(0,0,0,0)}.btn-warning:hover{color:#ffffff;background-color:#ec971f;border-color:rgba(0,0,0,0)}.btn-warning:active,.btn-warning.active,.open>.dropdown-toggle.btn-warning{color:#ffffff;background-color:#ec971f;border-color:rgba(0,0,0,0)}.btn-warning:active:hover,.btn-warning.active:hover,.open>.dropdown-toggle.btn-warning:hover,.btn-warning:active:focus,.btn-warning.active:focus,.open>.dropdown-toggle.btn-warning:focus,.btn-warning:active.focus,.btn-warning.active.focus,.open>.dropdown-toggle.btn-warning.focus{color:#ffffff;background-color:#d58512;border-color:rgba(0,0,0,0)}.btn-warning:active,.btn-warning.active,.open>.dropdown-toggle.btn-warning{background-image:none}.btn-warning.disabled:hover,.btn-warning[disabled]:hover,fieldset[disabled] .btn-warning:hover,.btn-warning.disabled:focus,.btn-warning[disabled]:focus,fieldset[disabled] .btn-warning:focus,.btn-warning.disabled.focus,.btn-warning[disabled].focus,fieldset[disabled] .btn-warning.focus{background-color:#f0ad4e;border-color:transparent}.btn-warning .badge{color:#f0ad4e;background-color:#ffffff}.btn-danger{color:#ffffff;background-color:#d9534f;border-color:transparent}.btn-danger:focus,.btn-danger.focus{color:#ffffff;background-color:#c9302c;border-color:rgba(0,0,0,0)}.btn-danger:hover{color:#ffffff;background-color:#c9302c;border-color:rgba(0,0,0,0)}.btn-danger:active,.btn-danger.active,.open>.dropdown-toggle.btn-danger{color:#ffffff;background-color:#c9302c;border-color:rgba(0,0,0,0)}.btn-danger:active:hover,.btn-danger.active:hover,.open>.dropdown-toggle.btn-danger:hover,.btn-danger:active:focus,.btn-danger.active:focus,.open>.dropdown-toggle.btn-danger:focus,.btn-danger:active.focus,.btn-danger.active.focus,.open>.dropdown-toggle.btn-danger.focus{color:#ffffff;background-color:#ac2925;border-color:rgba(0,0,0,0)}.btn-danger:active,.btn-danger.active,.open>.dropdown-toggle.btn-danger{background-image:none}.btn-danger.disabled:hover,.btn-danger[disabled]:hover,fieldset[disabled] .btn-danger:hover,.btn-danger.disabled:focus,.btn-danger[disabled]:focus,fieldset[disabled] .btn-danger:focus,.btn-danger.disabled.focus,.btn-danger[disabled].focus,fieldset[disabled] .btn-danger.focus{background-color:#d9534f;border-color:transparent}.btn-danger .badge{color:#d9534f;background-color:#ffffff}.btn-link{color:#991A1C;font-weight:normal;border-radius:0}.btn-link,.btn-link:active,.btn-link.active,.btn-link[disabled],fieldset[disabled] .btn-link{background-color:transparent;-webkit-box-shadow:none;box-shadow:none}.btn-link,.btn-link:hover,.btn-link:focus,.btn-link:active{border-color:transparent}.btn-link:hover,.btn-link:focus{color:#991A1C;text-decoration:underline;background-color:transparent}.btn-link[disabled]:hover,fieldset[disabled] .btn-link:hover,.btn-link[disabled]:focus,fieldset[disabled] .btn-link:focus{color:#4e5d6c;text-decoration:none}.btn-lg,.btn-group-lg>.btn{padding:12px 24px;font-size:19px;line-height:1.3333333;border-radius:0}.btn-sm,.btn-group-sm>.btn{padding:5px 10px;font-size:12px;line-height:1.5;border-radius:0}.btn-xs,.btn-group-xs>.btn{padding:1px 5px;font-size:12px;line-height:1.5;border-radius:0}.btn-block{display:block;width:100%}.btn-block+.btn-block{margin-top:5px}input[type="submit"].btn-block,input[type="reset"].btn-block,input[type="button"].btn-block{width:100%}.fade{opacity:0;-webkit-transition:opacity 0.15s linear;-o-transition:opacity 0.15s linear;transition:opacity 0.15s linear}.fade.in{opacity:1}.collapse{display:none}.collapse.in{display:block}tr.collapse.in{display:table-row}tbody.collapse.in{display:table-row-group}.collapsing{position:relative;height:0;overflow:hidden;-webkit-transition-property:height, visibility;-o-transition-property:height, visibility;transition-property:height, visibility;-webkit-transition-duration:0.35s;-o-transition-duration:0.35s;transition-duration:0.35s;-webkit-transition-timing-function:ease;-o-transition-timing-function:ease;transition-timing-function:ease}.caret{display:inline-block;width:0;height:0;margin-left:2px;vertical-align:middle;border-top:4px dashed;border-top:4px solid \9;border-right:4px solid transparent;border-left:4px solid transparent}.dropup,.dropdown{position:relative}.dropdown-toggle:focus{outline:0}.dropdown-menu{position:absolute;top:100%;left:0;z-index:1000;display:none;float:left;min-width:160px;padding:5px 0;margin:2px 0 0;list-style:none;font-size:15px;text-align:left;background-color:#4e5d6c;border:1px solid transparent;border-radius:0;-webkit-box-shadow:0 6px 12px rgba(0,0,0,0.175);box-shadow:0 6px 12px rgba(0,0,0,0.175);-webkit-background-clip:padding-box;background-clip:padding-box}.dropdown-menu.pull-right{right:0;left:auto}.dropdown-menu .divider{height:1px;margin:9.5px 0;overflow:hidden;background-color:#2b3e50}.dropdown-menu>li>a{display:block;padding:3px 20px;clear:both;font-weight:normal;line-height:1.42857143;color:#ebebeb;white-space:nowrap}.dropdown-menu>li>a:hover,.dropdown-menu>li>a:focus{text-decoration:none;color:#ebebeb;background-color:#485563}.dropdown-menu>.active>a,.dropdown-menu>.active>a:hover,.dropdown-menu>.active>a:focus{color:#ffffff;text-decoration:none;outline:0;background-color:#991A1C}.dropdown-menu>.disabled>a,.dropdown-menu>.disabled>a:hover,.dropdown-menu>.disabled>a:focus{color:#2b3e50}.dropdown-menu>.disabled>a:hover,.dropdown-menu>.disabled>a:focus{text-decoration:none;background-color:transparent;background-image:none;filter:progid:DXImageTransform.Microsoft.gradient(enabled=false);cursor:not-allowed}.open>.dropdown-menu{display:block}.open>a{outline:0}.dropdown-menu-right{left:auto;right:0}.dropdown-menu-left{left:0;right:auto}.dropdown-header{display:block;padding:3px 20px;font-size:12px;line-height:1.42857143;color:#2b3e50;white-space:nowrap}.dropdown-backdrop{position:fixed;left:0;right:0;bottom:0;top:0;z-index:990}.pull-right>.dropdown-menu{right:0;left:auto}.dropup .caret,.navbar-fixed-bottom .dropdown .caret{border-top:0;border-bottom:4px dashed;border-bottom:4px solid \9;content:""}.dropup .dropdown-menu,.navbar-fixed-bottom .dropdown .dropdown-menu{top:auto;bottom:100%;margin-bottom:2px}@media (min-width:768px){.navbar-right .dropdown-menu{left:auto;right:0}.navbar-right .dropdown-menu-left{left:0;right:auto}}.btn-group,.btn-group-vertical{position:relative;display:inline-block;vertical-align:middle}.btn-group>.btn,.btn-group-vertical>.btn{position:relative;float:left}.btn-group>.btn:hover,.btn-group-vertical>.btn:hover,.btn-group>.btn:focus,.btn-group-vertical>.btn:focus,.btn-group>.btn:active,.btn-group-vertical>.btn:active,.btn-group>.btn.active,.btn-group-vertical>.btn.active{z-index:2}.btn-group .btn+.btn,.btn-group .btn+.btn-group,.btn-group .btn-group+.btn,.btn-group .btn-group+.btn-group{margin-left:-1px}.btn-toolbar{margin-left:-5px}.btn-toolbar .btn,.btn-toolbar .btn-group,.btn-toolbar .input-group{float:left}.btn-toolbar>.btn,.btn-toolbar>.btn-group,.btn-toolbar>.input-group{margin-left:5px}.btn-group>.btn:not(:first-child):not(:last-child):not(.dropdown-toggle){border-radius:0}.btn-group>.btn:first-child{margin-left:0}.btn-group>.btn:first-child:not(:last-child):not(.dropdown-toggle){border-bottom-right-radius:0;border-top-right-radius:0}.btn-group>.btn:last-child:not(:first-child),.btn-group>.dropdown-toggle:not(:first-child){border-bottom-left-radius:0;border-top-left-radius:0}.btn-group>.btn-group{float:left}.btn-group>.btn-group:not(:first-child):not(:last-child)>.btn{border-radius:0}.btn-group>.btn-group:first-child:not(:last-child)>.btn:last-child,.btn-group>.btn-group:first-child:not(:last-child)>.dropdown-toggle{border-bottom-right-radius:0;border-top-right-radius:0}.btn-group>.btn-group:last-child:not(:first-child)>.btn:first-child{border-bottom-left-radius:0;border-top-left-radius:0}.btn-group .dropdown-toggle:active,.btn-group.open .dropdown-toggle{outline:0}.btn-group>.btn+.dropdown-toggle{padding-left:8px;padding-right:8px}.btn-group>.btn-lg+.dropdown-toggle{padding-left:12px;padding-right:12px}.btn-group.open .dropdown-toggle{-webkit-box-shadow:inset 0 3px 5px rgba(0,0,0,0.125);box-shadow:inset 0 3px 5px rgba(0,0,0,0.125)}.btn-group.open .dropdown-toggle.btn-link{-webkit-box-shadow:none;box-shadow:none}.btn .caret{margin-left:0}.btn-lg .caret{border-width:5px 5px 0;border-bottom-width:0}.dropup .btn-lg .caret{border-width:0 5px 5px}.btn-group-vertical>.btn,.btn-group-vertical>.btn-group,.btn-group-vertical>.btn-group>.btn{display:block;float:none;width:100%;max-width:100%}.btn-group-vertical>.btn-group>.btn{float:none}.btn-group-vertical>.btn+.btn,.btn-group-vertical>.btn+.btn-group,.btn-group-vertical>.btn-group+.btn,.btn-group-vertical>.btn-group+.btn-group{margin-top:-1px;margin-left:0}.btn-group-vertical>.btn:not(:first-child):not(:last-child){border-radius:0}.btn-group-vertical>.btn:first-child:not(:last-child){border-top-right-radius:0;border-top-left-radius:0;border-bottom-right-radius:0;border-bottom-left-radius:0}.btn-group-vertical>.btn:last-child:not(:first-child){border-top-right-radius:0;border-top-left-radius:0;border-bottom-right-radius:0;border-bottom-left-radius:0}.btn-group-vertical>.btn-group:not(:first-child):not(:last-child)>.btn{border-radius:0}.btn-group-vertical>.btn-group:first-child:not(:last-child)>.btn:last-child,.btn-group-vertical>.btn-group:first-child:not(:last-child)>.dropdown-toggle{border-bottom-right-radius:0;border-bottom-left-radius:0}.btn-group-vertical>.btn-group:last-child:not(:first-child)>.btn:first-child{border-top-right-radius:0;border-top-left-radius:0}.btn-group-justified{display:table;width:100%;table-layout:fixed;border-collapse:separate}.btn-group-justified>.btn,.btn-group-justified>.btn-group{float:none;display:table-cell;width:1%}.btn-group-justified>.btn-group .btn{width:100%}.btn-group-justified>.btn-group .dropdown-menu{left:auto}[data-toggle="buttons"]>.btn input[type="radio"],[data-toggle="buttons"]>.btn-group>.btn input[type="radio"],[data-toggle="buttons"]>.btn input[type="checkbox"],[data-toggle="buttons"]>.btn-group>.btn input[type="checkbox"]{position:absolute;clip:rect(0, 0, 0, 0);pointer-events:none}.input-group{position:relative;display:table;border-collapse:separate}.input-group[class*="col-"]{float:none;padding-left:0;padding-right:0}.input-group .form-control{position:relative;z-index:2;float:left;width:100%;margin-bottom:0}.input-group .form-control:focus{z-index:3}.input-group-lg>.form-control,.input-group-lg>.input-group-addon,.input-group-lg>.input-group-btn>.btn{height:52px;padding:12px 24px;font-size:19px;line-height:1.3333333;border-radius:0}select.input-group-lg>.form-control,select.input-group-lg>.input-group-addon,select.input-group-lg>.input-group-btn>.btn{height:52px;line-height:52px}textarea.input-group-lg>.form-control,textarea.input-group-lg>.input-group-addon,textarea.input-group-lg>.input-group-btn>.btn,select[multiple].input-group-lg>.form-control,select[multiple].input-group-lg>.input-group-addon,select[multiple].input-group-lg>.input-group-btn>.btn{height:auto}.input-group-sm>.form-control,.input-group-sm>.input-group-addon,.input-group-sm>.input-group-btn>.btn{height:30px;padding:5px 10px;font-size:12px;line-height:1.5;border-radius:0}select.input-group-sm>.form-control,select.input-group-sm>.input-group-addon,select.input-group-sm>.input-group-btn>.btn{height:30px;line-height:30px}textarea.input-group-sm>.form-control,textarea.input-group-sm>.input-group-addon,textarea.input-group-sm>.input-group-btn>.btn,select[multiple].input-group-sm>.form-control,select[multiple].input-group-sm>.input-group-addon,select[multiple].input-group-sm>.input-group-btn>.btn{height:auto}.input-group-addon,.input-group-btn,.input-group .form-control{display:table-cell}.input-group-addon:not(:first-child):not(:last-child),.input-group-btn:not(:first-child):not(:last-child),.input-group .form-control:not(:first-child):not(:last-child){border-radius:0}.input-group-addon,.input-group-btn{width:1%;white-space:nowrap;vertical-align:middle}.input-group-addon{padding:8px 16px;font-size:15px;font-weight:normal;line-height:1;color:#2b3e50;text-align:center;background-color:#4e5d6c;border:1px solid transparent;border-radius:0}.input-group-addon.input-sm{padding:5px 10px;font-size:12px;border-radius:0}.input-group-addon.input-lg{padding:12px 24px;font-size:19px;border-radius:0}.input-group-addon input[type="radio"],.input-group-addon input[type="checkbox"]{margin-top:0}.input-group .form-control:first-child,.input-group-addon:first-child,.input-group-btn:first-child>.btn,.input-group-btn:first-child>.btn-group>.btn,.input-group-btn:first-child>.dropdown-toggle,.input-group-btn:last-child>.btn:not(:last-child):not(.dropdown-toggle),.input-group-btn:last-child>.btn-group:not(:last-child)>.btn{border-bottom-right-radius:0;border-top-right-radius:0}.input-group-addon:first-child{border-right:0}.input-group .form-control:last-child,.input-group-addon:last-child,.input-group-btn:last-child>.btn,.input-group-btn:last-child>.btn-group>.btn,.input-group-btn:last-child>.dropdown-toggle,.input-group-btn:first-child>.btn:not(:first-child),.input-group-btn:first-child>.btn-group:not(:first-child)>.btn{border-bottom-left-radius:0;border-top-left-radius:0}.input-group-addon:last-child{border-left:0}.input-group-btn{position:relative;font-size:0;white-space:nowrap}.input-group-btn>.btn{position:relative}.input-group-btn>.btn+.btn{margin-left:-1px}.input-group-btn>.btn:hover,.input-group-btn>.btn:focus,.input-group-btn>.btn:active{z-index:2}.input-group-btn:first-child>.btn,.input-group-btn:first-child>.btn-group{margin-right:-1px}.input-group-btn:last-child>.btn,.input-group-btn:last-child>.btn-group{z-index:2;margin-left:-1px}.nav{margin-bottom:0;padding-left:0;list-style:none}.nav>li{position:relative;display:block}.nav>li>a{position:relative;display:block;padding:10px 15px}.nav>li>a:hover,.nav>li>a:focus{text-decoration:none;background-color:#4e5d6c}.nav>li.disabled>a{color:#4e5d6c}.nav>li.disabled>a:hover,.nav>li.disabled>a:focus{color:#4e5d6c;text-decoration:none;background-color:transparent;cursor:not-allowed}.nav .open>a,.nav .open>a:hover,.nav .open>a:focus{background-color:#4e5d6c;border-color:#991A1C}.nav .nav-divider{height:1px;margin:9.5px 0;overflow:hidden;background-color:#e5e5e5}.nav>li>a>img{max-width:none}.nav-tabs{border-bottom:1px solid transparent}.nav-tabs>li{float:left;margin-bottom:-1px}.nav-tabs>li>a{margin-right:2px;line-height:1.42857143;border:1px solid transparent;border-radius:0 0 0 0}.nav-tabs>li>a:hover{border-color:#4e5d6c #4e5d6c transparent}.nav-tabs>li.active>a,.nav-tabs>li.active>a:hover,.nav-tabs>li.active>a:focus{color:#ebebeb;background-color:#2b3e50;border:1px solid #4e5d6c;border-bottom-color:transparent;cursor:default}.nav-tabs.nav-justified{width:100%;border-bottom:0}.nav-tabs.nav-justified>li{float:none}.nav-tabs.nav-justified>li>a{text-align:center;margin-bottom:5px}.nav-tabs.nav-justified>.dropdown .dropdown-menu{top:auto;left:auto}@media (min-width:768px){.nav-tabs.nav-justified>li{display:table-cell;width:1%}.nav-tabs.nav-justified>li>a{margin-bottom:0}}.nav-tabs.nav-justified>li>a{margin-right:0;border-radius:0}.nav-tabs.nav-justified>.active>a,.nav-tabs.nav-justified>.active>a:hover,.nav-tabs.nav-justified>.active>a:focus{border:1px solid #4e5d6c}@media (min-width:768px){.nav-tabs.nav-justified>li>a{border-bottom:1px solid #4e5d6c;border-radius:0 0 0 0}.nav-tabs.nav-justified>.active>a,.nav-tabs.nav-justified>.active>a:hover,.nav-tabs.nav-justified>.active>a:focus{border-bottom-color:#4e5d6c}}.nav-pills>li{float:left}.nav-pills>li>a{border-radius:0}.nav-pills>li+li{margin-left:2px}.nav-pills>li.active>a,.nav-pills>li.active>a:hover,.nav-pills>li.active>a:focus{color:#ffffff;background-color:#991A1C}.nav-stacked>li{float:none}.nav-stacked>li+li{margin-top:2px;margin-left:0}.nav-justified{width:100%}.nav-justified>li{float:none}.nav-justified>li>a{text-align:center;margin-bottom:5px}.nav-justified>.dropdown .dropdown-menu{top:auto;left:auto}@media (min-width:768px){.nav-justified>li{display:table-cell;width:1%}.nav-justified>li>a{margin-bottom:0}}.nav-tabs-justified{border-bottom:0}.nav-tabs-justified>li>a{margin-right:0;border-radius:0}.nav-tabs-justified>.active>a,.nav-tabs-justified>.active>a:hover,.nav-tabs-justified>.active>a:focus{border:1px solid #4e5d6c}@media (min-width:768px){.nav-tabs-justified>li>a{border-bottom:1px solid #4e5d6c;border-radius:0 0 0 0}.nav-tabs-justified>.active>a,.nav-tabs-justified>.active>a:hover,.nav-tabs-justified>.active>a:focus{border-bottom-color:#4e5d6c}}.tab-content>.tab-pane{display:none}.tab-content>.active{display:block}.nav-tabs .dropdown-menu{margin-top:-1px;border-top-right-radius:0;border-top-left-radius:0}.navbar{position:relative;min-height:40px;margin-bottom:21px;border:1px solid transparent}@media (min-width:768px){.navbar{border-radius:0}}@media (min-width:768px){.navbar-header{float:left}}.navbar-collapse{overflow-x:visible;padding-right:15px;padding-left:15px;border-top:1px solid transparent;-webkit-box-shadow:inset 0 1px 0 rgba(255,255,255,0.1);box-shadow:inset 0 1px 0 rgba(255,255,255,0.1);-webkit-overflow-scrolling:touch}.navbar-collapse.in{overflow-y:auto}@media (min-width:768px){.navbar-collapse{width:auto;border-top:0;-webkit-box-shadow:none;box-shadow:none}.navbar-collapse.collapse{display:block !important;height:auto !important;padding-bottom:0;overflow:visible !important}.navbar-collapse.in{overflow-y:visible}.navbar-fixed-top .navbar-collapse,.navbar-static-top .navbar-collapse,.navbar-fixed-bottom .navbar-collapse{padding-left:0;padding-right:0}}.navbar-fixed-top .navbar-collapse,.navbar-fixed-bottom .navbar-collapse{max-height:340px}@media (max-device-width:480px) and (orientation:landscape){.navbar-fixed-top .navbar-collapse,.navbar-fixed-bottom .navbar-collapse{max-height:200px}}.container>.navbar-header,.container-fluid>.navbar-header,.container>.navbar-collapse,.container-fluid>.navbar-collapse{margin-right:-15px;margin-left:-15px}@media (min-width:768px){.container>.navbar-header,.container-fluid>.navbar-header,.container>.navbar-collapse,.container-fluid>.navbar-collapse{margin-right:0;margin-left:0}}.navbar-static-top{z-index:1000;border-width:0 0 1px}@media (min-width:768px){.navbar-static-top{border-radius:0}}.navbar-fixed-top,.navbar-fixed-bottom{position:fixed;right:0;left:0;z-index:1030}@media (min-width:768px){.navbar-fixed-top,.navbar-fixed-bottom{border-radius:0}}.navbar-fixed-top{top:0;border-width:0 0 1px}.navbar-fixed-bottom{bottom:0;margin-bottom:0;border-width:1px 0 0}.navbar-brand{float:left;padding:9.5px 15px;font-size:19px;line-height:21px;height:40px}.navbar-brand:hover,.navbar-brand:focus{text-decoration:none}.navbar-brand>img{display:block}@media (min-width:768px){.navbar>.container .navbar-brand,.navbar>.container-fluid .navbar-brand{margin-left:-15px}}.navbar-toggle{position:relative;float:right;margin-right:15px;padding:9px 10px;margin-top:3px;margin-bottom:3px;background-color:transparent;background-image:none;border:1px solid transparent;border-radius:0}.navbar-toggle:focus{outline:0}.navbar-toggle .icon-bar{display:block;width:22px;height:2px;border-radius:1px}.navbar-toggle .icon-bar+.icon-bar{margin-top:4px}@media (min-width:768px){.navbar-toggle{display:none}}.navbar-nav{margin:4.75px -15px}.navbar-nav>li>a{padding-top:10px;padding-bottom:10px;line-height:21px}@media (max-width:767px){.navbar-nav .open .dropdown-menu{position:static;float:none;width:auto;margin-top:0;background-color:transparent;border:0;-webkit-box-shadow:none;box-shadow:none}.navbar-nav .open .dropdown-menu>li>a,.navbar-nav .open .dropdown-menu .dropdown-header{padding:5px 15px 5px 25px}.navbar-nav .open .dropdown-menu>li>a{line-height:21px}.navbar-nav .open .dropdown-menu>li>a:hover,.navbar-nav .open .dropdown-menu>li>a:focus{background-image:none}}@media (min-width:768px){.navbar-nav{float:left;margin:0}.navbar-nav>li{float:left}.navbar-nav>li>a{padding-top:9.5px;padding-bottom:9.5px}}.navbar-form{margin-left:-15px;margin-right:-15px;padding:10px 15px;border-top:1px solid transparent;border-bottom:1px solid transparent;-webkit-box-shadow:inset 0 1px 0 rgba(255,255,255,0.1),0 1px 0 rgba(255,255,255,0.1);box-shadow:inset 0 1px 0 rgba(255,255,255,0.1),0 1px 0 rgba(255,255,255,0.1);margin-top:0.5px;margin-bottom:0.5px}@media (min-width:768px){.navbar-form .form-group{display:inline-block;margin-bottom:0;vertical-align:middle}.navbar-form .form-control{display:inline-block;width:auto;vertical-align:middle}.navbar-form .form-control-static{display:inline-block}.navbar-form .input-group{display:inline-table;vertical-align:middle}.navbar-form .input-group .input-group-addon,.navbar-form .input-group .input-group-btn,.navbar-form .input-group .form-control{width:auto}.navbar-form .input-group>.form-control{width:100%}.navbar-form .control-label{margin-bottom:0;vertical-align:middle}.navbar-form .radio,.navbar-form .checkbox{display:inline-block;margin-top:0;margin-bottom:0;vertical-align:middle}.navbar-form .radio label,.navbar-form .checkbox label{padding-left:0}.navbar-form .radio input[type="radio"],.navbar-form .checkbox input[type="checkbox"]{position:relative;margin-left:0}.navbar-form .has-feedback .form-control-feedback{top:0}}@media (max-width:767px){.navbar-form .form-group{margin-bottom:5px}.navbar-form .form-group:last-child{margin-bottom:0}}@media (min-width:768px){.navbar-form{width:auto;border:0;margin-left:0;margin-right:0;padding-top:0;padding-bottom:0;-webkit-box-shadow:none;box-shadow:none}}.navbar-nav>li>.dropdown-menu{margin-top:0;border-top-right-radius:0;border-top-left-radius:0}.navbar-fixed-bottom .navbar-nav>li>.dropdown-menu{margin-bottom:0;border-top-right-radius:0;border-top-left-radius:0;border-bottom-right-radius:0;border-bottom-left-radius:0}.navbar-btn{margin-top:0.5px;margin-bottom:0.5px}.navbar-btn.btn-sm{margin-top:5px;margin-bottom:5px}.navbar-btn.btn-xs{margin-top:9px;margin-bottom:9px}.navbar-text{margin-top:9.5px;margin-bottom:9.5px}@media (min-width:768px){.navbar-text{float:left;margin-left:15px;margin-right:15px}}@media (min-width:768px){.navbar-left{float:left !important}.navbar-right{float:right !important;margin-right:-15px}.navbar-right~.navbar-right{margin-right:0}}.navbar-default{background-color:#4e5d6c;border-color:transparent}.navbar-default .navbar-brand{color:#ebebeb}.navbar-default .navbar-brand:hover,.navbar-default .navbar-brand:focus{color:#ebebeb;background-color:transparent}.navbar-default .navbar-text{color:#ebebeb}.navbar-default .navbar-nav>li>a{color:#ebebeb}.navbar-default .navbar-nav>li>a:hover,.navbar-default .navbar-nav>li>a:focus{color:#ebebeb;background-color:#485563}.navbar-default .navbar-nav>.active>a,.navbar-default .navbar-nav>.active>a:hover,.navbar-default .navbar-nav>.active>a:focus{color:#ebebeb;background-color:#485563}.navbar-default .navbar-nav>.disabled>a,.navbar-default .navbar-nav>.disabled>a:hover,.navbar-default .navbar-nav>.disabled>a:focus{color:#cccccc;background-color:transparent}.navbar-default .navbar-toggle{border-color:transparent}.navbar-default .navbar-toggle:hover,.navbar-default .navbar-toggle:focus{background-color:#485563}.navbar-default .navbar-toggle .icon-bar{background-color:#ebebeb}.navbar-default .navbar-collapse,.navbar-default .navbar-form{border-color:transparent}.navbar-default .navbar-nav>.open>a,.navbar-default .navbar-nav>.open>a:hover,.navbar-default .navbar-nav>.open>a:focus{background-color:#485563;color:#ebebeb}@media (max-width:767px){.navbar-default .navbar-nav .open .dropdown-menu>li>a{color:#ebebeb}.navbar-default .navbar-nav .open .dropdown-menu>li>a:hover,.navbar-default .navbar-nav .open .dropdown-menu>li>a:focus{color:#ebebeb;background-color:#485563}.navbar-default .navbar-nav .open .dropdown-menu>.active>a,.navbar-default .navbar-nav .open .dropdown-menu>.active>a:hover,.navbar-default .navbar-nav .open .dropdown-menu>.active>a:focus{color:#ebebeb;background-color:#485563}.navbar-default .navbar-nav .open .dropdown-menu>.disabled>a,.navbar-default .navbar-nav .open .dropdown-menu>.disabled>a:hover,.navbar-default .navbar-nav .open .dropdown-menu>.disabled>a:focus{color:#cccccc;background-color:transparent}}.navbar-default .navbar-link{color:#ebebeb}.navbar-default .navbar-link:hover{color:#ebebeb}.navbar-default .btn-link{color:#ebebeb}.navbar-default .btn-link:hover,.navbar-default .btn-link:focus{color:#ebebeb}.navbar-default .btn-link[disabled]:hover,fieldset[disabled] .navbar-default .btn-link:hover,.navbar-default .btn-link[disabled]:focus,fieldset[disabled] .navbar-default .btn-link:focus{color:#cccccc}.navbar-inverse{background-color:#991A1C;border-color:transparent}.navbar-inverse .navbar-brand{color:#ebebeb}.navbar-inverse .navbar-brand:hover,.navbar-inverse .navbar-brand:focus{color:#ebebeb;background-color:transparent}.navbar-inverse .navbar-text{color:#ebebeb}.navbar-inverse .navbar-nav>li>a{color:#ebebeb}.navbar-inverse .navbar-nav>li>a:hover,.navbar-inverse .navbar-nav>li>a:focus{color:#ebebeb;background-color:#c85e17}.navbar-inverse .navbar-nav>.active>a,.navbar-inverse .navbar-nav>.active>a:hover,.navbar-inverse .navbar-nav>.active>a:focus{color:#ebebeb;background-color:#c85e17}.navbar-inverse .navbar-nav>.disabled>a,.navbar-inverse .navbar-nav>.disabled>a:hover,.navbar-inverse .navbar-nav>.disabled>a:focus{color:#444444;background-color:transparent}.navbar-inverse .navbar-toggle{border-color:transparent}.navbar-inverse .navbar-toggle:hover,.navbar-inverse .navbar-toggle:focus{background-color:#c85e17}.navbar-inverse .navbar-toggle .icon-bar{background-color:#ebebeb}.navbar-inverse .navbar-collapse,.navbar-inverse .navbar-form{border-color:#bf5a16}.navbar-inverse .navbar-nav>.open>a,.navbar-inverse .navbar-nav>.open>a:hover,.navbar-inverse .navbar-nav>.open>a:focus{background-color:#c85e17;color:#ebebeb}@media (max-width:767px){.navbar-inverse .navbar-nav .open .dropdown-menu>.dropdown-header{border-color:transparent}.navbar-inverse .navbar-nav .open .dropdown-menu .divider{background-color:transparent}.navbar-inverse .navbar-nav .open .dropdown-menu>li>a{color:#ebebeb}.navbar-inverse .navbar-nav .open .dropdown-menu>li>a:hover,.navbar-inverse .navbar-nav .open .dropdown-menu>li>a:focus{color:#ebebeb;background-color:#c85e17}.navbar-inverse .navbar-nav .open .dropdown-menu>.active>a,.navbar-inverse .navbar-nav .open .dropdown-menu>.active>a:hover,.navbar-inverse .navbar-nav .open .dropdown-menu>.active>a:focus{color:#ebebeb;background-color:#c85e17}.navbar-inverse .navbar-nav .open .dropdown-menu>.disabled>a,.navbar-inverse .navbar-nav .open .dropdown-menu>.disabled>a:hover,.navbar-inverse .navbar-nav .open .dropdown-menu>.disabled>a:focus{color:#444444;background-color:transparent}}.navbar-inverse .navbar-link{color:#ebebeb}.navbar-inverse .navbar-link:hover{color:#ebebeb}.navbar-inverse .btn-link{color:#ebebeb}.navbar-inverse .btn-link:hover,.navbar-inverse .btn-link:focus{color:#ebebeb}.navbar-inverse .btn-link[disabled]:hover,fieldset[disabled] .navbar-inverse .btn-link:hover,.navbar-inverse .btn-link[disabled]:focus,fieldset[disabled] .navbar-inverse .btn-link:focus{color:#444444}.breadcrumb{padding:8px 15px;margin-bottom:21px;list-style:none;background-color:#4e5d6c;border-radius:0}.breadcrumb>li{display:inline-block}.breadcrumb>li+li:before{content:"/\00a0";padding:0 5px;color:#ebebeb}.breadcrumb>.active{color:#ebebeb}.pagination{display:inline-block;padding-left:0;margin:21px 0;border-radius:0}.pagination>li{display:inline}.pagination>li>a,.pagination>li>span{position:relative;float:left;padding:8px 16px;line-height:1.42857143;text-decoration:none;color:#ebebeb;background-color:#4e5d6c;border:1px solid transparent;margin-left:-1px}.pagination>li:first-child>a,.pagination>li:first-child>span{margin-left:0;border-bottom-left-radius:0;border-top-left-radius:0}.pagination>li:last-child>a,.pagination>li:last-child>span{border-bottom-right-radius:0;border-top-right-radius:0}.pagination>li>a:hover,.pagination>li>span:hover,.pagination>li>a:focus,.pagination>li>span:focus{z-index:2;color:#ebebeb;background-color:#485563;border-color:transparent}.pagination>.active>a,.pagination>.active>span,.pagination>.active>a:hover,.pagination>.active>span:hover,.pagination>.active>a:focus,.pagination>.active>span:focus{z-index:3;color:#ebebeb;background-color:#991A1C;border-color:transparent;cursor:default}.pagination>.disabled>span,.pagination>.disabled>span:hover,.pagination>.disabled>span:focus,.pagination>.disabled>a,.pagination>.disabled>a:hover,.pagination>.disabled>a:focus{color:#323c46;background-color:#4e5d6c;border-color:transparent;cursor:not-allowed}.pagination-lg>li>a,.pagination-lg>li>span{padding:12px 24px;font-size:19px;line-height:1.3333333}.pagination-lg>li:first-child>a,.pagination-lg>li:first-child>span{border-bottom-left-radius:0;border-top-left-radius:0}.pagination-lg>li:last-child>a,.pagination-lg>li:last-child>span{border-bottom-right-radius:0;border-top-right-radius:0}.pagination-sm>li>a,.pagination-sm>li>span{padding:5px 10px;font-size:12px;line-height:1.5}.pagination-sm>li:first-child>a,.pagination-sm>li:first-child>span{border-bottom-left-radius:0;border-top-left-radius:0}.pagination-sm>li:last-child>a,.pagination-sm>li:last-child>span{border-bottom-right-radius:0;border-top-right-radius:0}.pager{padding-left:0;margin:21px 0;list-style:none;text-align:center}.pager li{display:inline}.pager li>a,.pager li>span{display:inline-block;padding:5px 14px;background-color:#4e5d6c;border:1px solid transparent;border-radius:15px}.pager li>a:hover,.pager li>a:focus{text-decoration:none;background-color:#485563}.pager .next>a,.pager .next>span{float:right}.pager .previous>a,.pager .previous>span{float:left}.pager .disabled>a,.pager .disabled>a:hover,.pager .disabled>a:focus,.pager .disabled>span{color:#323c46;background-color:#4e5d6c;cursor:not-allowed}.label{display:inline;padding:.2em .6em .3em;font-size:75%;font-weight:bold;line-height:1;color:#ffffff;text-align:center;white-space:nowrap;vertical-align:baseline;border-radius:.25em}a.label:hover,a.label:focus{color:#ffffff;text-decoration:none;cursor:pointer}.label:empty{display:none}.btn .label{position:relative;top:-1px}.label-default{background-color:#4e5d6c}.label-default[href]:hover,.label-default[href]:focus{background-color:#39444e}.label-primary{background-color:#991A1C}.label-primary[href]:hover,.label-primary[href]:focus{background-color:#b15315}.label-success{background-color:#5cb85c}.label-success[href]:hover,.label-success[href]:focus{background-color:#449d44}.label-info{background-color:#5bc0de}.label-info[href]:hover,.label-info[href]:focus{background-color:#31b0d5}.label-warning{background-color:#f0ad4e}.label-warning[href]:hover,.label-warning[href]:focus{background-color:#ec971f}.label-danger{background-color:#d9534f}.label-danger[href]:hover,.label-danger[href]:focus{background-color:#c9302c}.badge{display:inline-block;min-width:10px;padding:3px 7px;font-size:12px;font-weight:300;color:#ebebeb;line-height:1;vertical-align:middle;white-space:nowrap;text-align:center;background-color:#4e5d6c;border-radius:10px}.badge:empty{display:none}.btn .badge{position:relative;top:-1px}.btn-xs .badge,.btn-group-xs>.btn .badge{top:0;padding:1px 5px}a.badge:hover,a.badge:focus{color:#ffffff;text-decoration:none;cursor:pointer}.list-group-item.active>.badge,.nav-pills>.active>a>.badge{color:#991A1C;background-color:#ffffff}.list-group-item>.badge{float:right}.list-group-item>.badge+.badge{margin-right:5px}.nav-pills>li>a>.badge{margin-left:3px}.jumbotron{padding-top:30px;padding-bottom:30px;margin-bottom:30px;color:inherit;background-color:#4e5d6c}.jumbotron h1,.jumbotron .h1{color:inherit}.jumbotron p{margin-bottom:15px;font-size:23px;font-weight:200}.jumbotron>hr{border-top-color:#39444e}.container .jumbotron,.container-fluid .jumbotron{border-radius:0;padding-left:15px;padding-right:15px}.jumbotron .container{max-width:100%}@media screen and (min-width:768px){.jumbotron{padding-top:48px;padding-bottom:48px}.container .jumbotron,.container-fluid .jumbotron{padding-left:60px;padding-right:60px}.jumbotron h1,.jumbotron .h1{font-size:68px}}.thumbnail{display:block;padding:4px;margin-bottom:21px;line-height:1.42857143;background-color:#2b3e50;border:1px solid #dddddd;border-radius:0;-webkit-transition:border .2s ease-in-out;-o-transition:border .2s ease-in-out;transition:border .2s ease-in-out}.thumbnail>img,.thumbnail a>img{margin-left:auto;margin-right:auto}a.thumbnail:hover,a.thumbnail:focus,a.thumbnail.active{border-color:#991A1C}.thumbnail .caption{padding:9px;color:#ebebeb}.alert{padding:15px;margin-bottom:21px;border:1px solid transparent;border-radius:0}.alert h4{margin-top:0;color:inherit}.alert .alert-link{font-weight:bold}.alert>p,.alert>ul{margin-bottom:0}.alert>p+p{margin-top:5px}.alert-dismissable,.alert-dismissible{padding-right:35px}.alert-dismissable .close,.alert-dismissible .close{position:relative;top:-2px;right:-21px;color:inherit}.alert-success{background-color:#5cb85c;border-color:transparent;color:#ebebeb}.alert-success hr{border-top-color:rgba(0,0,0,0)}.alert-success .alert-link{color:#d2d2d2}.alert-info{background-color:#5bc0de;border-color:transparent;color:#ebebeb}.alert-info hr{border-top-color:rgba(0,0,0,0)}.alert-info .alert-link{color:#d2d2d2}.alert-warning{background-color:#f0ad4e;border-color:transparent;color:#ebebeb}.alert-warning hr{border-top-color:rgba(0,0,0,0)}.alert-warning .alert-link{color:#d2d2d2}.alert-danger{background-color:#d9534f;border-color:transparent;color:#ebebeb}.alert-danger hr{border-top-color:rgba(0,0,0,0)}.alert-danger .alert-link{color:#d2d2d2}@-webkit-keyframes progress-bar-stripes{from{background-position:40px 0}to{background-position:0 0}}@-o-keyframes progress-bar-stripes{from{background-position:40px 0}to{background-position:0 0}}@keyframes progress-bar-stripes{from{background-position:40px 0}to{background-position:0 0}}.progress{overflow:hidden;height:21px;margin-bottom:21px;background-color:#4e5d6c;border-radius:0;-webkit-box-shadow:inset 0 1px 2px rgba(0,0,0,0.1);box-shadow:inset 0 1px 2px rgba(0,0,0,0.1)}.progress-bar{float:left;width:0%;height:100%;font-size:12px;line-height:21px;color:#ffffff;text-align:center;background-color:#991A1C;-webkit-box-shadow:inset 0 -1px 0 rgba(0,0,0,0.15);box-shadow:inset 0 -1px 0 rgba(0,0,0,0.15);-webkit-transition:width 0.6s ease;-o-transition:width 0.6s ease;transition:width 0.6s ease}.progress-striped .progress-bar,.progress-bar-striped{background-image:-webkit-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:-o-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);-webkit-background-size:40px 40px;background-size:40px 40px}.progress.active .progress-bar,.progress-bar.active{-webkit-animation:progress-bar-stripes 2s linear infinite;-o-animation:progress-bar-stripes 2s linear infinite;animation:progress-bar-stripes 2s linear infinite}.progress-bar-success{background-color:#5cb85c}.progress-striped .progress-bar-success{background-image:-webkit-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:-o-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent)}.progress-bar-info{background-color:#5bc0de}.progress-striped .progress-bar-info{background-image:-webkit-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:-o-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent)}.progress-bar-warning{background-color:#f0ad4e}.progress-striped .progress-bar-warning{background-image:-webkit-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:-o-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent)}.progress-bar-danger{background-color:#d9534f}.progress-striped .progress-bar-danger{background-image:-webkit-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:-o-linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent);background-image:linear-gradient(45deg, rgba(255,255,255,0.15) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.15) 50%, rgba(255,255,255,0.15) 75%, transparent 75%, transparent)}.media{margin-top:15px}.media:first-child{margin-top:0}.media,.media-body{zoom:1;overflow:hidden}.media-body{width:10000px}.media-object{display:block}.media-object.img-thumbnail{max-width:none}.media-right,.media>.pull-right{padding-left:10px}.media-left,.media>.pull-left{padding-right:10px}.media-left,.media-right,.media-body{display:table-cell;vertical-align:top}.media-middle{vertical-align:middle}.media-bottom{vertical-align:bottom}.media-heading{margin-top:0;margin-bottom:5px}.media-list{padding-left:0;list-style:none}.list-group{margin-bottom:20px;padding-left:0}.list-group-item{position:relative;display:block;padding:10px 15px;margin-bottom:-1px;background-color:#4e5d6c;border:1px solid transparent}.list-group-item:first-child{border-top-right-radius:0;border-top-left-radius:0}.list-group-item:last-child{margin-bottom:0;border-bottom-right-radius:0;border-bottom-left-radius:0}a.list-group-item,button.list-group-item{color:#ebebeb}a.list-group-item .list-group-item-heading,button.list-group-item .list-group-item-heading{color:#ebebeb}a.list-group-item:hover,button.list-group-item:hover,a.list-group-item:focus,button.list-group-item:focus{text-decoration:none;color:#ebebeb;background-color:#485563}button.list-group-item{width:100%;text-align:left}.list-group-item.disabled,.list-group-item.disabled:hover,.list-group-item.disabled:focus{background-color:#ebebeb;color:#4e5d6c;cursor:not-allowed}.list-group-item.disabled .list-group-item-heading,.list-group-item.disabled:hover .list-group-item-heading,.list-group-item.disabled:focus .list-group-item-heading{color:inherit}.list-group-item.disabled .list-group-item-text,.list-group-item.disabled:hover .list-group-item-text,.list-group-item.disabled:focus .list-group-item-text{color:#4e5d6c}.list-group-item.active,.list-group-item.active:hover,.list-group-item.active:focus{z-index:2;color:#ffffff;background-color:#991A1C;border-color:#991A1C}.list-group-item.active .list-group-item-heading,.list-group-item.active:hover .list-group-item-heading,.list-group-item.active:focus .list-group-item-heading,.list-group-item.active .list-group-item-heading>small,.list-group-item.active:hover .list-group-item-heading>small,.list-group-item.active:focus .list-group-item-heading>small,.list-group-item.active .list-group-item-heading>.small,.list-group-item.active:hover .list-group-item-heading>.small,.list-group-item.active:focus .list-group-item-heading>.small{color:inherit}.list-group-item.active .list-group-item-text,.list-group-item.active:hover .list-group-item-text,.list-group-item.active:focus .list-group-item-text{color:#f9decc}.list-group-item-success{color:#ebebeb;background-color:#5cb85c}a.list-group-item-success,button.list-group-item-success{color:#ebebeb}a.list-group-item-success .list-group-item-heading,button.list-group-item-success .list-group-item-heading{color:inherit}a.list-group-item-success:hover,button.list-group-item-success:hover,a.list-group-item-success:focus,button.list-group-item-success:focus{color:#ebebeb;background-color:#4cae4c}a.list-group-item-success.active,button.list-group-item-success.active,a.list-group-item-success.active:hover,button.list-group-item-success.active:hover,a.list-group-item-success.active:focus,button.list-group-item-success.active:focus{color:#fff;background-color:#ebebeb;border-color:#ebebeb}.list-group-item-info{color:#ebebeb;background-color:#5bc0de}a.list-group-item-info,button.list-group-item-info{color:#ebebeb}a.list-group-item-info .list-group-item-heading,button.list-group-item-info .list-group-item-heading{color:inherit}a.list-group-item-info:hover,button.list-group-item-info:hover,a.list-group-item-info:focus,button.list-group-item-info:focus{color:#ebebeb;background-color:#46b8da}a.list-group-item-info.active,button.list-group-item-info.active,a.list-group-item-info.active:hover,button.list-group-item-info.active:hover,a.list-group-item-info.active:focus,button.list-group-item-info.active:focus{color:#fff;background-color:#ebebeb;border-color:#ebebeb}.list-group-item-warning{color:#ebebeb;background-color:#f0ad4e}a.list-group-item-warning,button.list-group-item-warning{color:#ebebeb}a.list-group-item-warning .list-group-item-heading,button.list-group-item-warning .list-group-item-heading{color:inherit}a.list-group-item-warning:hover,button.list-group-item-warning:hover,a.list-group-item-warning:focus,button.list-group-item-warning:focus{color:#ebebeb;background-color:#eea236}a.list-group-item-warning.active,button.list-group-item-warning.active,a.list-group-item-warning.active:hover,button.list-group-item-warning.active:hover,a.list-group-item-warning.active:focus,button.list-group-item-warning.active:focus{color:#fff;background-color:#ebebeb;border-color:#ebebeb}.list-group-item-danger{color:#ebebeb;background-color:#d9534f}a.list-group-item-danger,button.list-group-item-danger{color:#ebebeb}a.list-group-item-danger .list-group-item-heading,button.list-group-item-danger .list-group-item-heading{color:inherit}a.list-group-item-danger:hover,button.list-group-item-danger:hover,a.list-group-item-danger:focus,button.list-group-item-danger:focus{color:#ebebeb;background-color:#d43f3a}a.list-group-item-danger.active,button.list-group-item-danger.active,a.list-group-item-danger.active:hover,button.list-group-item-danger.active:hover,a.list-group-item-danger.active:focus,button.list-group-item-danger.active:focus{color:#fff;background-color:#ebebeb;border-color:#ebebeb}.list-group-item-heading{margin-top:0;margin-bottom:5px}.list-group-item-text{margin-bottom:0;line-height:1.3}.panel{margin-bottom:21px;background-color:#546575;border:1px solid transparent;border-radius:0;-webkit-box-shadow:0 1px 1px rgba(0,0,0,0.05);box-shadow:0 1px 1px rgba(0,0,0,0.05)}.panel-body{padding:15px}.panel-heading{padding:10px 15px;border-bottom:1px solid transparent;border-top-right-radius:-1;border-top-left-radius:-1}.panel-heading>.dropdown .dropdown-toggle{color:inherit}.panel-title{margin-top:0;margin-bottom:0;font-size:17px;color:inherit}.panel-title>a,.panel-title>small,.panel-title>.small,.panel-title>small>a,.panel-title>.small>a{color:inherit}.panel-footer{padding:10px 15px;background-color:#485563;border-top:1px solid transparent;border-bottom-right-radius:-1;border-bottom-left-radius:-1}.panel>.list-group,.panel>.panel-collapse>.list-group{margin-bottom:0}.panel>.list-group .list-group-item,.panel>.panel-collapse>.list-group .list-group-item{border-width:1px 0;border-radius:0}.panel>.list-group:first-child .list-group-item:first-child,.panel>.panel-collapse>.list-group:first-child .list-group-item:first-child{border-top:0;border-top-right-radius:-1;border-top-left-radius:-1}.panel>.list-group:last-child .list-group-item:last-child,.panel>.panel-collapse>.list-group:last-child .list-group-item:last-child{border-bottom:0;border-bottom-right-radius:-1;border-bottom-left-radius:-1}.panel>.panel-heading+.panel-collapse>.list-group .list-group-item:first-child{border-top-right-radius:0;border-top-left-radius:0}.panel-heading+.list-group .list-group-item:first-child{border-top-width:0}.list-group+.panel-footer{border-top-width:0}.panel>.table,.panel>.table-responsive>.table,.panel>.panel-collapse>.table{margin-bottom:0}.panel>.table caption,.panel>.table-responsive>.table caption,.panel>.panel-collapse>.table caption{padding-left:15px;padding-right:15px}.panel>.table:first-child,.panel>.table-responsive:first-child>.table:first-child{border-top-right-radius:-1;border-top-left-radius:-1}.panel>.table:first-child>thead:first-child>tr:first-child,.panel>.table-responsive:first-child>.table:first-child>thead:first-child>tr:first-child,.panel>.table:first-child>tbody:first-child>tr:first-child,.panel>.table-responsive:first-child>.table:first-child>tbody:first-child>tr:first-child{border-top-left-radius:-1;border-top-right-radius:-1}.panel>.table:first-child>thead:first-child>tr:first-child td:first-child,.panel>.table-responsive:first-child>.table:first-child>thead:first-child>tr:first-child td:first-child,.panel>.table:first-child>tbody:first-child>tr:first-child td:first-child,.panel>.table-responsive:first-child>.table:first-child>tbody:first-child>tr:first-child td:first-child,.panel>.table:first-child>thead:first-child>tr:first-child th:first-child,.panel>.table-responsive:first-child>.table:first-child>thead:first-child>tr:first-child th:first-child,.panel>.table:first-child>tbody:first-child>tr:first-child th:first-child,.panel>.table-responsive:first-child>.table:first-child>tbody:first-child>tr:first-child th:first-child{border-top-left-radius:-1}.panel>.table:first-child>thead:first-child>tr:first-child td:last-child,.panel>.table-responsive:first-child>.table:first-child>thead:first-child>tr:first-child td:last-child,.panel>.table:first-child>tbody:first-child>tr:first-child td:last-child,.panel>.table-responsive:first-child>.table:first-child>tbody:first-child>tr:first-child td:last-child,.panel>.table:first-child>thead:first-child>tr:first-child th:last-child,.panel>.table-responsive:first-child>.table:first-child>thead:first-child>tr:first-child th:last-child,.panel>.table:first-child>tbody:first-child>tr:first-child th:last-child,.panel>.table-responsive:first-child>.table:first-child>tbody:first-child>tr:first-child th:last-child{border-top-right-radius:-1}.panel>.table:last-child,.panel>.table-responsive:last-child>.table:last-child{border-bottom-right-radius:-1;border-bottom-left-radius:-1}.panel>.table:last-child>tbody:last-child>tr:last-child,.panel>.table-responsive:last-child>.table:last-child>tbody:last-child>tr:last-child,.panel>.table:last-child>tfoot:last-child>tr:last-child,.panel>.table-responsive:last-child>.table:last-child>tfoot:last-child>tr:last-child{border-bottom-left-radius:-1;border-bottom-right-radius:-1}.panel>.table:last-child>tbody:last-child>tr:last-child td:first-child,.panel>.table-responsive:last-child>.table:last-child>tbody:last-child>tr:last-child td:first-child,.panel>.table:last-child>tfoot:last-child>tr:last-child td:first-child,.panel>.table-responsive:last-child>.table:last-child>tfoot:last-child>tr:last-child td:first-child,.panel>.table:last-child>tbody:last-child>tr:last-child th:first-child,.panel>.table-responsive:last-child>.table:last-child>tbody:last-child>tr:last-child th:first-child,.panel>.table:last-child>tfoot:last-child>tr:last-child th:first-child,.panel>.table-responsive:last-child>.table:last-child>tfoot:last-child>tr:last-child th:first-child{border-bottom-left-radius:-1}.panel>.table:last-child>tbody:last-child>tr:last-child td:last-child,.panel>.table-responsive:last-child>.table:last-child>tbody:last-child>tr:last-child td:last-child,.panel>.table:last-child>tfoot:last-child>tr:last-child td:last-child,.panel>.table-responsive:last-child>.table:last-child>tfoot:last-child>tr:last-child td:last-child,.panel>.table:last-child>tbody:last-child>tr:last-child th:last-child,.panel>.table-responsive:last-child>.table:last-child>tbody:last-child>tr:last-child th:last-child,.panel>.table:last-child>tfoot:last-child>tr:last-child th:last-child,.panel>.table-responsive:last-child>.table:last-child>tfoot:last-child>tr:last-child th:last-child{border-bottom-right-radius:-1}.panel>.panel-body+.table,.panel>.panel-body+.table-responsive,.panel>.table+.panel-body,.panel>.table-responsive+.panel-body{border-top:1px solid #4e5d6c}.panel>.table>tbody:first-child>tr:first-child th,.panel>.table>tbody:first-child>tr:first-child td{border-top:0}.panel>.table-bordered,.panel>.table-responsive>.table-bordered{border:0}.panel>.table-bordered>thead>tr>th:first-child,.panel>.table-responsive>.table-bordered>thead>tr>th:first-child,.panel>.table-bordered>tbody>tr>th:first-child,.panel>.table-responsive>.table-bordered>tbody>tr>th:first-child,.panel>.table-bordered>tfoot>tr>th:first-child,.panel>.table-responsive>.table-bordered>tfoot>tr>th:first-child,.panel>.table-bordered>thead>tr>td:first-child,.panel>.table-responsive>.table-bordered>thead>tr>td:first-child,.panel>.table-bordered>tbody>tr>td:first-child,.panel>.table-responsive>.table-bordered>tbody>tr>td:first-child,.panel>.table-bordered>tfoot>tr>td:first-child,.panel>.table-responsive>.table-bordered>tfoot>tr>td:first-child{border-left:0}.panel>.table-bordered>thead>tr>th:last-child,.panel>.table-responsive>.table-bordered>thead>tr>th:last-child,.panel>.table-bordered>tbody>tr>th:last-child,.panel>.table-responsive>.table-bordered>tbody>tr>th:last-child,.panel>.table-bordered>tfoot>tr>th:last-child,.panel>.table-responsive>.table-bordered>tfoot>tr>th:last-child,.panel>.table-bordered>thead>tr>td:last-child,.panel>.table-responsive>.table-bordered>thead>tr>td:last-child,.panel>.table-bordered>tbody>tr>td:last-child,.panel>.table-responsive>.table-bordered>tbody>tr>td:last-child,.panel>.table-bordered>tfoot>tr>td:last-child,.panel>.table-responsive>.table-bordered>tfoot>tr>td:last-child{border-right:0}.panel>.table-bordered>thead>tr:first-child>td,.panel>.table-responsive>.table-bordered>thead>tr:first-child>td,.panel>.table-bordered>tbody>tr:first-child>td,.panel>.table-responsive>.table-bordered>tbody>tr:first-child>td,.panel>.table-bordered>thead>tr:first-child>th,.panel>.table-responsive>.table-bordered>thead>tr:first-child>th,.panel>.table-bordered>tbody>tr:first-child>th,.panel>.table-responsive>.table-bordered>tbody>tr:first-child>th{border-bottom:0}.panel>.table-bordered>tbody>tr:last-child>td,.panel>.table-responsive>.table-bordered>tbody>tr:last-child>td,.panel>.table-bordered>tfoot>tr:last-child>td,.panel>.table-responsive>.table-bordered>tfoot>tr:last-child>td,.panel>.table-bordered>tbody>tr:last-child>th,.panel>.table-responsive>.table-bordered>tbody>tr:last-child>th,.panel>.table-bordered>tfoot>tr:last-child>th,.panel>.table-responsive>.table-bordered>tfoot>tr:last-child>th{border-bottom:0}.panel>.table-responsive{border:0;margin-bottom:0}.panel-group{margin-bottom:21px}.panel-group .panel{margin-bottom:0;border-radius:0}.panel-group .panel+.panel{margin-top:5px}.panel-group .panel-heading{border-bottom:0}.panel-group .panel-heading+.panel-collapse>.panel-body,.panel-group .panel-heading+.panel-collapse>.list-group{border-top:1px solid transparent}.panel-group .panel-footer{border-top:0}.panel-group .panel-footer+.panel-collapse .panel-body{border-bottom:1px solid transparent}.panel-default{border-color:transparent}.panel-default>.panel-heading{color:#333333;background-color:#f5f5f5;border-color:transparent}.panel-default>.panel-heading+.panel-collapse>.panel-body{border-top-color:transparent}.panel-default>.panel-heading .badge{color:#f5f5f5;background-color:#333333}.panel-default>.panel-footer+.panel-collapse>.panel-body{border-bottom-color:transparent}.panel-primary{border-color:transparent}.panel-primary>.panel-heading{color:#ffffff;background-color:#991A1C;border-color:transparent}.panel-primary>.panel-heading+.panel-collapse>.panel-body{border-top-color:transparent}.panel-primary>.panel-heading .badge{color:#991A1C;background-color:#ffffff}.panel-primary>.panel-footer+.panel-collapse>.panel-body{border-bottom-color:transparent}.panel-success{border-color:transparent}.panel-success>.panel-heading{color:#ebebeb;background-color:#5cb85c;border-color:transparent}.panel-success>.panel-heading+.panel-collapse>.panel-body{border-top-color:transparent}.panel-success>.panel-heading .badge{color:#5cb85c;background-color:#ebebeb}.panel-success>.panel-footer+.panel-collapse>.panel-body{border-bottom-color:transparent}.panel-info{border-color:transparent}.panel-info>.panel-heading{color:#ebebeb;background-color:#5bc0de;border-color:transparent}.panel-info>.panel-heading+.panel-collapse>.panel-body{border-top-color:transparent}.panel-info>.panel-heading .badge{color:#5bc0de;background-color:#ebebeb}.panel-info>.panel-footer+.panel-collapse>.panel-body{border-bottom-color:transparent}.panel-warning{border-color:transparent}.panel-warning>.panel-heading{color:#ebebeb;background-color:#f0ad4e;border-color:transparent}.panel-warning>.panel-heading+.panel-collapse>.panel-body{border-top-color:transparent}.panel-warning>.panel-heading .badge{color:#f0ad4e;background-color:#ebebeb}.panel-warning>.panel-footer+.panel-collapse>.panel-body{border-bottom-color:transparent}.panel-danger{border-color:transparent}.panel-danger>.panel-heading{color:#ebebeb;background-color:#d9534f;border-color:transparent}.panel-danger>.panel-heading+.panel-collapse>.panel-body{border-top-color:transparent}.panel-danger>.panel-heading .badge{color:#d9534f;background-color:#ebebeb}.panel-danger>.panel-footer+.panel-collapse>.panel-body{border-bottom-color:transparent}.embed-responsive{position:relative;display:block;height:0;padding:0;overflow:hidden}.embed-responsive .embed-responsive-item,.embed-responsive iframe,.embed-responsive embed,.embed-responsive object,.embed-responsive video{position:absolute;top:0;left:0;bottom:0;height:100%;width:100%;border:0}.embed-responsive-16by9{padding-bottom:56.25%}.embed-responsive-4by3{padding-bottom:75%}.well{min-height:20px;padding:19px;margin-bottom:20px;background-color:#546575;border:1px solid transparent;border-radius:0;-webkit-box-shadow:inset 0 1px 1px rgba(0,0,0,0.05);box-shadow:inset 0 1px 1px rgba(0,0,0,0.05)}.well blockquote{border-color:#ddd;border-color:rgba(0,0,0,0.15)}.well-lg{padding:24px;border-radius:0}.well-sm{padding:9px;border-radius:0}.close{float:right;font-size:22.5px;font-weight:bold;line-height:1;color:#ebebeb;text-shadow:none;opacity:0.2;filter:alpha(opacity=20)}.close:hover,.close:focus{color:#ebebeb;text-decoration:none;cursor:pointer;opacity:0.5;filter:alpha(opacity=50)}button.close{padding:0;cursor:pointer;background:transparent;border:0;-webkit-appearance:none}.modal-open{overflow:hidden}.modal{display:none;overflow:hidden;position:fixed;top:0;right:0;bottom:0;left:0;z-index:1050;-webkit-overflow-scrolling:touch;outline:0}.modal.fade .modal-dialog{-webkit-transform:translate(0, -25%);-ms-transform:translate(0, -25%);-o-transform:translate(0, -25%);transform:translate(0, -25%);-webkit-transition:-webkit-transform .3s ease-out;-o-transition:-o-transform .3s ease-out;transition:transform .3s ease-out}.modal.in .modal-dialog{-webkit-transform:translate(0, 0);-ms-transform:translate(0, 0);-o-transform:translate(0, 0);transform:translate(0, 0)}.modal-open .modal{overflow-x:hidden;overflow-y:auto}.modal-dialog{position:relative;width:auto;margin:10px}.modal-content{position:relative;background-color:#4e5d6c;border:1px solid transparent;border-radius:0;-webkit-box-shadow:0 3px 9px rgba(0,0,0,0.5);box-shadow:0 3px 9px rgba(0,0,0,0.5);-webkit-background-clip:padding-box;background-clip:padding-box;outline:0}.modal-backdrop{position:fixed;top:0;right:0;bottom:0;left:0;z-index:1040;background-color:#000000}.modal-backdrop.fade{opacity:0;filter:alpha(opacity=0)}.modal-backdrop.in{opacity:0.5;filter:alpha(opacity=50)}.modal-header{padding:15px;border-bottom:1px solid #2b3e50}.modal-header .close{margin-top:-2px}.modal-title{margin:0;line-height:1.42857143}.modal-body{position:relative;padding:20px}.modal-footer{padding:20px;text-align:right;border-top:1px solid #2b3e50}.modal-footer .btn+.btn{margin-left:5px;margin-bottom:0}.modal-footer .btn-group .btn+.btn{margin-left:-1px}.modal-footer .btn-block+.btn-block{margin-left:0}.modal-scrollbar-measure{position:absolute;top:-9999px;width:50px;height:50px;overflow:scroll}@media (min-width:768px){.modal-dialog{width:600px;margin:30px auto}.modal-content{-webkit-box-shadow:0 5px 15px rgba(0,0,0,0.5);box-shadow:0 5px 15px rgba(0,0,0,0.5)}.modal-sm{width:300px}}@media (min-width:992px){.modal-lg{width:900px}}.tooltip{position:absolute;z-index:1070;display:block;font-family:"Lato","Helvetica Neue",Helvetica,Arial,sans-serif;font-style:normal;font-weight:normal;letter-spacing:normal;line-break:auto;line-height:1.42857143;text-align:left;text-align:start;text-decoration:none;text-shadow:none;text-transform:none;white-space:normal;word-break:normal;word-spacing:normal;word-wrap:normal;font-size:12px;opacity:0;filter:alpha(opacity=0)}.tooltip.in{opacity:0.9;filter:alpha(opacity=90)}.tooltip.top{margin-top:-3px;padding:5px 0}.tooltip.right{margin-left:3px;padding:0 5px}.tooltip.bottom{margin-top:3px;padding:5px 0}.tooltip.left{margin-left:-3px;padding:0 5px}.tooltip-inner{max-width:200px;padding:3px 8px;color:#ffffff;text-align:center;background-color:#000000;border-radius:0}.tooltip-arrow{position:absolute;width:0;height:0;border-color:transparent;border-style:solid}.tooltip.top .tooltip-arrow{bottom:0;left:50%;margin-left:-5px;border-width:5px 5px 0;border-top-color:#000000}.tooltip.top-left .tooltip-arrow{bottom:0;right:5px;margin-bottom:-5px;border-width:5px 5px 0;border-top-color:#000000}.tooltip.top-right .tooltip-arrow{bottom:0;left:5px;margin-bottom:-5px;border-width:5px 5px 0;border-top-color:#000000}.tooltip.right .tooltip-arrow{top:50%;left:0;margin-top:-5px;border-width:5px 5px 5px 0;border-right-color:#000000}.tooltip.left .tooltip-arrow{top:50%;right:0;margin-top:-5px;border-width:5px 0 5px 5px;border-left-color:#000000}.tooltip.bottom .tooltip-arrow{top:0;left:50%;margin-left:-5px;border-width:0 5px 5px;border-bottom-color:#000000}.tooltip.bottom-left .tooltip-arrow{top:0;right:5px;margin-top:-5px;border-width:0 5px 5px;border-bottom-color:#000000}.tooltip.bottom-right .tooltip-arrow{top:0;left:5px;margin-top:-5px;border-width:0 5px 5px;border-bottom-color:#000000}.popover{position:absolute;top:0;left:0;z-index:1060;display:none;max-width:276px;padding:1px;font-family:"Lato","Helvetica Neue",Helvetica,Arial,sans-serif;font-style:normal;font-weight:normal;letter-spacing:normal;line-break:auto;line-height:1.42857143;text-align:left;text-align:start;text-decoration:none;text-shadow:none;text-transform:none;white-space:normal;word-break:normal;word-spacing:normal;word-wrap:normal;font-size:15px;background-color:#4e5d6c;-webkit-background-clip:padding-box;background-clip:padding-box;border:1px solid transparent;border-radius:0;-webkit-box-shadow:0 5px 10px rgba(0,0,0,0.2);box-shadow:0 5px 10px rgba(0,0,0,0.2)}.popover.top{margin-top:-10px}.popover.right{margin-left:10px}.popover.bottom{margin-top:10px}.popover.left{margin-left:-10px}.popover-title{margin:0;padding:8px 14px;font-size:15px;background-color:#485563;border-bottom:1px solid #3d4954;border-radius:-1 -1 0 0}.popover-content{padding:9px 14px}.popover>.arrow,.popover>.arrow:after{position:absolute;display:block;width:0;height:0;border-color:transparent;border-style:solid}.popover>.arrow{border-width:11px}.popover>.arrow:after{border-width:10px;content:""}.popover.top>.arrow{left:50%;margin-left:-11px;border-bottom-width:0;border-top-color:transparent;bottom:-11px}.popover.top>.arrow:after{content:" ";bottom:1px;margin-left:-10px;border-bottom-width:0;border-top-color:#4e5d6c}.popover.right>.arrow{top:50%;left:-11px;margin-top:-11px;border-left-width:0;border-right-color:transparent}.popover.right>.arrow:after{content:" ";left:1px;bottom:-10px;border-left-width:0;border-right-color:#4e5d6c}.popover.bottom>.arrow{left:50%;margin-left:-11px;border-top-width:0;border-bottom-color:transparent;top:-11px}.popover.bottom>.arrow:after{content:" ";top:1px;margin-left:-10px;border-top-width:0;border-bottom-color:#4e5d6c}.popover.left>.arrow{top:50%;right:-11px;margin-top:-11px;border-right-width:0;border-left-color:transparent}.popover.left>.arrow:after{content:" ";right:1px;border-right-width:0;border-left-color:#4e5d6c;bottom:-10px}.carousel{position:relative}.carousel-inner{position:relative;overflow:hidden;width:100%}.carousel-inner>.item{display:none;position:relative;-webkit-transition:.6s ease-in-out left;-o-transition:.6s ease-in-out left;transition:.6s ease-in-out left}.carousel-inner>.item>img,.carousel-inner>.item>a>img{line-height:1}@media all and (transform-3d),(-webkit-transform-3d){.carousel-inner>.item{-webkit-transition:-webkit-transform .6s ease-in-out;-o-transition:-o-transform .6s ease-in-out;transition:transform .6s ease-in-out;-webkit-backface-visibility:hidden;backface-visibility:hidden;-webkit-perspective:1000px;perspective:1000px}.carousel-inner>.item.next,.carousel-inner>.item.active.right{-webkit-transform:translate3d(100%, 0, 0);transform:translate3d(100%, 0, 0);left:0}.carousel-inner>.item.prev,.carousel-inner>.item.active.left{-webkit-transform:translate3d(-100%, 0, 0);transform:translate3d(-100%, 0, 0);left:0}.carousel-inner>.item.next.left,.carousel-inner>.item.prev.right,.carousel-inner>.item.active{-webkit-transform:translate3d(0, 0, 0);transform:translate3d(0, 0, 0);left:0}}.carousel-inner>.active,.carousel-inner>.next,.carousel-inner>.prev{display:block}.carousel-inner>.active{left:0}.carousel-inner>.next,.carousel-inner>.prev{position:absolute;top:0;width:100%}.carousel-inner>.next{left:100%}.carousel-inner>.prev{left:-100%}.carousel-inner>.next.left,.carousel-inner>.prev.right{left:0}.carousel-inner>.active.left{left:-100%}.carousel-inner>.active.right{left:100%}.carousel-control{position:absolute;top:0;left:0;bottom:0;width:15%;opacity:0.5;filter:alpha(opacity=50);font-size:20px;color:#ffffff;text-align:center;text-shadow:0 1px 2px rgba(0,0,0,0.6);background-color:rgba(0,0,0,0)}.carousel-control.left{background-image:-webkit-linear-gradient(left, rgba(0,0,0,0.5) 0, rgba(0,0,0,0.0001) 100%);background-image:-o-linear-gradient(left, rgba(0,0,0,0.5) 0, rgba(0,0,0,0.0001) 100%);background-image:-webkit-gradient(linear, left top, right top, from(rgba(0,0,0,0.5)), to(rgba(0,0,0,0.0001)));background-image:linear-gradient(to right, rgba(0,0,0,0.5) 0, rgba(0,0,0,0.0001) 100%);background-repeat:repeat-x;filter:progid:DXImageTransform.Microsoft.gradient(startColorstr='#80000000', endColorstr='#00000000', GradientType=1)}.carousel-control.right{left:auto;right:0;background-image:-webkit-linear-gradient(left, rgba(0,0,0,0.0001) 0, rgba(0,0,0,0.5) 100%);background-image:-o-linear-gradient(left, rgba(0,0,0,0.0001) 0, rgba(0,0,0,0.5) 100%);background-image:-webkit-gradient(linear, left top, right top, from(rgba(0,0,0,0.0001)), to(rgba(0,0,0,0.5)));background-image:linear-gradient(to right, rgba(0,0,0,0.0001) 0, rgba(0,0,0,0.5) 100%);background-repeat:repeat-x;filter:progid:DXImageTransform.Microsoft.gradient(startColorstr='#00000000', endColorstr='#80000000', GradientType=1)}.carousel-control:hover,.carousel-control:focus{outline:0;color:#ffffff;text-decoration:none;opacity:0.9;filter:alpha(opacity=90)}.carousel-control .icon-prev,.carousel-control .icon-next,.carousel-control .glyphicon-chevron-left,.carousel-control .glyphicon-chevron-right{position:absolute;top:50%;margin-top:-10px;z-index:5;display:inline-block}.carousel-control .icon-prev,.carousel-control .glyphicon-chevron-left{left:50%;margin-left:-10px}.carousel-control .icon-next,.carousel-control .glyphicon-chevron-right{right:50%;margin-right:-10px}.carousel-control .icon-prev,.carousel-control .icon-next{width:20px;height:20px;line-height:1;font-family:serif}.carousel-control .icon-prev:before{content:'\2039'}.carousel-control .icon-next:before{content:'\203a'}.carousel-indicators{position:absolute;bottom:10px;left:50%;z-index:15;width:60%;margin-left:-30%;padding-left:0;list-style:none;text-align:center}.carousel-indicators li{display:inline-block;width:10px;height:10px;margin:1px;text-indent:-999px;border:1px solid #ffffff;border-radius:10px;cursor:pointer;background-color:#000 \9;background-color:rgba(0,0,0,0)}.carousel-indicators .active{margin:0;width:12px;height:12px;background-color:#ffffff}.carousel-caption{position:absolute;left:15%;right:15%;bottom:20px;z-index:10;padding-top:20px;padding-bottom:20px;color:#ffffff;text-align:center;text-shadow:0 1px 2px rgba(0,0,0,0.6)}.carousel-caption .btn{text-shadow:none}@media screen and (min-width:768px){.carousel-control .glyphicon-chevron-left,.carousel-control .glyphicon-chevron-right,.carousel-control .icon-prev,.carousel-control .icon-next{width:30px;height:30px;margin-top:-10px;font-size:30px}.carousel-control .glyphicon-chevron-left,.carousel-control .icon-prev{margin-left:-10px}.carousel-control .glyphicon-chevron-right,.carousel-control .icon-next{margin-right:-10px}.carousel-caption{left:20%;right:20%;padding-bottom:30px}.carousel-indicators{bottom:20px}}.clearfix:before,.clearfix:after,.dl-horizontal dd:before,.dl-horizontal dd:after,.container:before,.container:after,.container-fluid:before,.container-fluid:after,.row:before,.row:after,.form-horizontal .form-group:before,.form-horizontal .form-group:after,.btn-toolbar:before,.btn-toolbar:after,.btn-group-vertical>.btn-group:before,.btn-group-vertical>.btn-group:after,.nav:before,.nav:after,.navbar:before,.navbar:after,.navbar-header:before,.navbar-header:after,.navbar-collapse:before,.navbar-collapse:after,.pager:before,.pager:after,.panel-body:before,.panel-body:after,.modal-header:before,.modal-header:after,.modal-footer:before,.modal-footer:after{content:" ";display:table}.clearfix:after,.dl-horizontal dd:after,.container:after,.container-fluid:after,.row:after,.form-horizontal .form-group:after,.btn-toolbar:after,.btn-group-vertical>.btn-group:after,.nav:after,.navbar:after,.navbar-header:after,.navbar-collapse:after,.pager:after,.panel-body:after,.modal-header:after,.modal-footer:after{clear:both}.center-block{display:block;margin-left:auto;margin-right:auto}.pull-right{float:right !important}.pull-left{float:left !important}.hide{display:none !important}.show{display:block !important}.invisible{visibility:hidden}.text-hide{font:0/0 a;color:transparent;text-shadow:none;background-color:transparent;border:0}.hidden{display:none !important}.affix{position:fixed}@-ms-viewport{width:device-width}.visible-xs,.visible-sm,.visible-md,.visible-lg{display:none !important}.visible-xs-block,.visible-xs-inline,.visible-xs-inline-block,.visible-sm-block,.visible-sm-inline,.visible-sm-inline-block,.visible-md-block,.visible-md-inline,.visible-md-inline-block,.visible-lg-block,.visible-lg-inline,.visible-lg-inline-block{display:none !important}@media (max-width:767px){.visible-xs{display:block !important}table.visible-xs{display:table !important}tr.visible-xs{display:table-row !important}th.visible-xs,td.visible-xs{display:table-cell !important}}@media (max-width:767px){.visible-xs-block{display:block !important}}@media (max-width:767px){.visible-xs-inline{display:inline !important}}@media (max-width:767px){.visible-xs-inline-block{display:inline-block !important}}@media (min-width:768px) and (max-width:991px){.visible-sm{display:block !important}table.visible-sm{display:table !important}tr.visible-sm{display:table-row !important}th.visible-sm,td.visible-sm{display:table-cell !important}}@media (min-width:768px) and (max-width:991px){.visible-sm-block{display:block !important}}@media (min-width:768px) and (max-width:991px){.visible-sm-inline{display:inline !important}}@media (min-width:768px) and (max-width:991px){.visible-sm-inline-block{display:inline-block !important}}@media (min-width:992px) and (max-width:1199px){.visible-md{display:block !important}table.visible-md{display:table !important}tr.visible-md{display:table-row !important}th.visible-md,td.visible-md{display:table-cell !important}}@media (min-width:992px) and (max-width:1199px){.visible-md-block{display:block !important}}@media (min-width:992px) and (max-width:1199px){.visible-md-inline{display:inline !important}}@media (min-width:992px) and (max-width:1199px){.visible-md-inline-block{display:inline-block !important}}@media (min-width:1200px){.visible-lg{display:block !important}table.visible-lg{display:table !important}tr.visible-lg{display:table-row !important}th.visible-lg,td.visible-lg{display:table-cell !important}}@media (min-width:1200px){.visible-lg-block{display:block !important}}@media (min-width:1200px){.visible-lg-inline{display:inline !important}}@media (min-width:1200px){.visible-lg-inline-block{display:inline-block !important}}@media (max-width:767px){.hidden-xs{display:none !important}}@media (min-width:768px) and (max-width:991px){.hidden-sm{display:none !important}}@media (min-width:992px) and (max-width:1199px){.hidden-md{display:none !important}}@media (min-width:1200px){.hidden-lg{display:none !important}}.visible-print{display:none !important}@media print{.visible-print{display:block !important}table.visible-print{display:table !important}tr.visible-print{display:table-row !important}th.visible-print,td.visible-print{display:table-cell !important}}.visible-print-block{display:none !important}@media print{.visible-print-block{display:block !important}}.visible-print-inline{display:none !important}@media print{.visible-print-inline{display:inline !important}}.visible-print-inline-block{display:none !important}@media print{.visible-print-inline-block{display:inline-block !important}}@media print{.hidden-print{display:none !important}}.navbar{-webkit-box-shadow:none;box-shadow:none;border:none;font-size:12px}.navbar-default .badge{background-color:#fff;color:#4e5d6c}.navbar-inverse .badge{background-color:#fff;color:#991A1C}.btn-default:hover{background-color:#485563}.btn-sm,.btn-xs{font-size:12px}.text-primary,.text-primary:hover{color:#991A1C}.text-success,.text-success:hover{color:#5cb85c}.text-danger,.text-danger:hover{color:#d9534f}.text-warning,.text-warning:hover{color:#f0ad4e}.text-info,.text-info:hover{color:#5bc0de}.page-header{border-bottom-color:#4e5d6c}.dropdown-menu{border:none;margin:0;-webkit-box-shadow:none;box-shadow:none}.dropdown-menu>li>a{font-size:12px}.btn-group.open .dropdown-toggle{-webkit-box-shadow:none;box-shadow:none}.dropdown-header{font-size:12px}table,.table{font-size:12px}table a:not(.btn),.table a:not(.btn){color:#fff;text-decoration:underline}table .dropdown-menu a,.table .dropdown-menu a{text-decoration:none}table .text-muted,.table .text-muted{color:#4e5d6c}table>thead>tr>th,.table>thead>tr>th,table>tbody>tr>th,.table>tbody>tr>th,table>tfoot>tr>th,.table>tfoot>tr>th,table>thead>tr>td,.table>thead>tr>td,table>tbody>tr>td,.table>tbody>tr>td,table>tfoot>tr>td,.table>tfoot>tr>td{border-color:transparent}input,textarea{color:#2b3e50}label,.radio label,.checkbox label,.help-block{font-size:12px}.input-addon,.input-group-addon{color:#ebebeb}.has-warning .help-block,.has-warning .control-label,.has-warning .radio,.has-warning .checkbox,.has-warning .radio-inline,.has-warning .checkbox-inline,.has-warning.radio label,.has-warning.checkbox label,.has-warning.radio-inline label,.has-warning.checkbox-inline label,.has-warning .form-control-feedback{color:#f0ad4e}.has-warning .form-control,.has-warning .form-control:focus{border:4px solid #f0ad4e}.has-warning .input-group-addon{border:none}.has-error .help-block,.has-error .control-label,.has-error .radio,.has-error .checkbox,.has-error .radio-inline,.has-error .checkbox-inline,.has-error.radio label,.has-error.checkbox label,.has-error.radio-inline label,.has-error.checkbox-inline label,.has-error .form-control-feedback{color:#d9534f}.has-error .form-control,.has-error .form-control:focus{border:4px solid #d9534f}.has-error .input-group-addon{border:none}.has-success .help-block,.has-success .control-label,.has-success .radio,.has-success .checkbox,.has-success .radio-inline,.has-success .checkbox-inline,.has-success.radio label,.has-success.checkbox label,.has-success.radio-inline label,.has-success.checkbox-inline label,.has-success .form-control-feedback{color:#5cb85c}.has-success .form-control,.has-success .form-control:focus{border:4px solid #5cb85c}.has-success .input-group-addon{border:none}.form-control:focus{-webkit-box-shadow:none;box-shadow:none}.has-warning .form-control:focus,.has-error .form-control:focus,.has-success .form-control:focus{-webkit-box-shadow:none;box-shadow:none}.nav .open>a,.nav .open>a:hover,.nav .open>a:focus{border-color:transparent}.nav-tabs>li>a{color:#ebebeb}.nav-pills>li>a{color:#ebebeb}.pager a{color:#ebebeb}.alert{color:#fff}.alert a,.alert .alert-link{color:#fff}.close{opacity:0.4}.close:hover,.close:focus{opacity:1}.well{-webkit-box-shadow:none;box-shadow:none}a.list-group-item.active,a.list-group-item.active:hover,a.list-group-item.active:focus{border:none}a.list-group-item-success.active{background-color:#5cb85c}a.list-group-item-success.active:hover,a.list-group-item-success.active:focus{background-color:#4cae4c}a.list-group-item-warning.active{background-color:#f0ad4e}a.list-group-item-warning.active:hover,a.list-group-item-warning.active:focus{background-color:#eea236}a.list-group-item-danger.active{background-color:#d9534f}a.list-group-item-danger.active:hover,a.list-group-item-danger.active:focus{background-color:#d43f3a}.panel{border:none}.panel-default>.panel-heading{background-color:#485563;color:#ebebeb}.thumbnail{background-color:#546575;border:none}.modal{padding:0}.modal-header,.modal-footer{background-color:#485563;border:none;border-radius:0}.popover-title{border:none}

)";

static const std::string home_html =
    R"(
<style>
	#mem_pool_rows tr td:first-child {
		text-align: left;
	}
	.value {
		color: #1B80E7;
		font-weight: 600;
		display: block;
		float: right;
	}
</style>

<div class="row">
	<div class="col-sm-12 col-md-12">
		<div class="panel panel-default" id="network-stats">
		  <div class="panel-heading">
			<h3 class="panel-title"><i class="fa fa-tasks fa-fw" aria-hidden="true"></i> Stats</h3>
		  </div>
		  <div class="panel-body">
			<div class="row">

				<div class="col-sm-6 col-md-3">
					<ul class="list-group" style="margin-bottom: 0px;">

						<li class="list-group-item d-flex justify-content-between align-items-center" data-toggle="tooltip" data-placement="top" data-original-title="Blockchain height, total amount of blocks starting from zero."><i class="fa fa-list-ol"></i> Height: <span id="networkHeight" class="value"></span></li>

						<li class="list-group-item d-flex justify-content-between align-items-center" data-toggle="tooltip" data-placement="top" data-original-title="The number of transactions in the network (excluding coinbase, i.e. reward for mined blocks)."><i class="fa fa fa-exchange"></i> Transactions: <span id="networkTransactions" class="value"></span></li>

						<li class="list-group-item d-flex justify-content-between align-items-center" data-toggle="tooltip" data-placement="top" data-original-title="Current Base Reward (for last mined block)."><i class="fa fa-certificate"></i> Reward: <span id="currentReward" class="value"></span></li>

					</ul>
				</div>
				<div class="col-sm-6 col-md-3">
					<ul class="list-group" style="margin-bottom: 0px;">

						<li class="list-group-item d-flex justify-content-between align-items-center" data-toggle="tooltip" data-placement="top" data-original-title="Total GoldenDoge supply in circulation."><i class="fa fa-money"></i> Supply: <span id="totalCoins" class="value"></span></li>

						<li class="list-group-item d-flex justify-content-between align-items-center" data-toggle="tooltip" data-placement="top" data-original-title="Percent of already emitted coins related to Initial supply before Tail emission."><i class="fa fa-university" aria-hidden="true"></i> Emission: <span id="emissionPercent" class="value"></span></li>

						<li class="list-group-item d-flex justify-content-between align-items-center" data-toggle="tooltip" data-placement="top" data-original-title="Minimum transaction fee."><i class="fa fa-money" aria-hidden="true"></i> Min. fee: <span id="minimumFee" class="value"></span></li>

					</ul>
				</div>
				<div class="col-sm-6 col-md-3">
					<ul class="list-group" style="margin-bottom: 0px;">

						<li class="list-group-item d-flex justify-content-between align-items-center" data-toggle="tooltip" data-placement="top" data-original-title="Difficulty for next block. Ratio at which at the current hashing speed blocks will be mined with 30 seconds interval."><i class="fa fa-unlock-alt"></i> Next Difficulty: <span id="networkDifficulty" class="value"></span></li>

						<li class="list-group-item d-flex justify-content-between align-items-center" data-toggle="tooltip" data-placement="top" data-original-title="Average difficulty by last 30 blocks."><i class="fa fa-lock"></i> Average Difficulty: <span id="avgDifficulty" class="value"></span></li>

						<li class="list-group-item d-flex justify-content-between align-items-center" data-toggle="tooltip" data-placement="top" data-original-title="Current estimated network hash rate. Calculated by current difficulty."><i class="fa fa-tachometer"></i> Hash Rate: <span id="networkHashrate" class="value"></span></li>

					</ul>
				</div>
				<div class="col-sm-6 col-md-3">
					<ul class="list-group" style="margin-bottom: 0px;">

						<li class="list-group-item d-flex justify-content-between align-items-center" data-toggle="tooltip" data-placement="top" data-original-title="Average estimated network hash rate. Calculated by average difficulty."><i class="fa fa-tachometer"></i> Avg. Hash Rate: <span id="avgHashrate" class="value"></span></li>

						<li class="list-group-item d-flex justify-content-between align-items-center" data-toggle="tooltip" data-placement="top" data-original-title="Estimated block solve time at estimated network speed and current difficulty."><i class="fa fa-clock-o" aria-hidden="true"></i> Est. solve time: <span id="blockSolveTime" class="value"></span></li>

						<li class="list-group-item d-flex justify-content-between align-items-center" data-toggle="tooltip" data-placement="top" data-original-title="Average solve time for the recent blocks listed below."><i class="fa fa-clock-o" aria-hidden="true"></i> Avg. solve time: <span id="avgSolveTime" class="value"></span></li>

					</ul>
				</div>


			</div>
		  </div>
		</div>
	</div>
</div>

<div class="row">
	<div class="col-md-12">
		<div class="panel panel-default">
		  <div class="panel-heading">

			<h3 class="panel-title"><i class="fa fa-area-chart" aria-hidden="true"></i> Charts

			<span class="text-default" data-toggle="tooltip" data-placement="right" data-original-title="Difficulty based on last blocks from the list below. Block size, transactions count. Load more blocks to enlarge chart range."><i class="fa fa-question-circle"></i></span>
			</h3>

		  </div>

            <div class="panel-body chart-wrapper">
              <canvas id="difficultyChart" height="210"></canvas>
            </div>

		</div>
	</div>
</div>

<div class="panel panel-default">
  <div class="panel-heading">

	<h3 class="panel-title"><i class="fa fa-exchange fa-fw" aria-hidden="true"></i> Transaction pool
	<span class="text-default" data-toggle="tooltip" data-placement="right" data-original-title="Recent transactions waiting to be included into a block. Once it happens a transaction gets into the blockchain and becomes confirmed."><i class="fa fa-question-circle"></i></span>
    </h3>

  </div>
  <div class="panel-body">

		<div class="row">
		  <div class="col-xs-6 col-sm-6 col-md-3 col-lg-3">
			<div class="panel panel-default tab-panel">
			  <div class="panel-body">

				  <ul class="nav nav-pills nav-stacked">
					<li data-toggle="tooltip" data-placement="bottom" data-original-title="">Count: <span id="mempool_count" class="value"></span></li>
				  </ul>

			  </div>
			</div>
		  </div>
		  <div class="col-xs-6 col-sm-6 col-md-3 col-lg-3">
			<div class="panel panel-default tab-panel">
			  <div class="panel-body">

				  <ul class="nav nav-pills nav-stacked">
					<li data-toggle="tooltip" data-placement="bottom" data-original-title="">Total Amount Out: <span id="mempool_amount" class="value"></span></li>
				  </ul>

			  </div>
			</div>
		  </div>
		  <div class="col-xs-6 col-sm-6 col-md-3 col-lg-3">
			<div class="panel panel-default tab-panel">
			  <div class="panel-body">

				  <ul class="nav nav-pills nav-stacked">
					<li href="javascript:void(0);" data-toggle="tooltip" data-placement="bottom" data-original-title="">Total Fees: <span id="mempool_fees" class="value"></span></a></li>
				  </ul>

			  </div>
			</div>
		  </div>
		  <div class="col-xs-6 col-sm-6 col-md-3 col-lg-3">
		    <div class="panel panel-default tab-panel">
		      <div class="panel-body">

			      <ul class="nav nav-pills nav-stacked">
				    <li data-toggle="tooltip" data-placement="bottom" data-original-title="">Total Size: <span id="mempool_sizes" class="value"></span></li>
			      </ul>

		      </div>
			</div>
		  </div>
		</div>


		<div class="table-responsive">
			<table class="table table-hover" id="mem_pool_table">
				<thead>
				<tr>
					<th width="30%"><i class="fa fa-clock-o"></i> Date &amp; time</th>
					<th width="15%"><i class="fa fa-money"></i> Amount out</th>
					<th width="10%"><i class="fa fa-tag"></i> Fee</th>
					<th width="10%"><i class="fa fa-archive"></i> Size</th>
					<th width="40%"><i class="fa fa-paw"></i> Hash</th>
				</tr>
				</thead>
				<tbody id="mem_pool_rows">

				</tbody>
			</table>
		</div>
	</div>
</div>


<div class="panel panel-default">
  <div class="panel-heading">
	<h3 class="panel-title"><i class="fa fa-chain fa-fw" aria-hidden="true"></i> Recent blocks</h3>
  </div>
  <div class="panel-body">
	<div class="row">
		<div class="col-sm-8 col-md-6 col-lg-5">
			<div class="input-group">
			
			</div>
		</div>
	</div>

	<div class="table-responsive">
		<table class="table table-hover">
			<thead>
			<tr>
				<th><i class="fa fa-bars"></i> Height</th>
				<th><i class="fa fa-clock-o"></i> Date &amp; time</th>
				<th><i class="fa fa-archive"></i> Size</th>
				<th><i class="fa fa-paw"></i> Block Hash</th>
				<th><i class="fa fa-unlock-alt"></i> Difficulty</th>
				<th><i class="fa fa fa-exchange"></i> Txs</th>
			</tr>
			</thead>
			<tbody id="blocks_rows">

			</tbody>
		</table>
	</div>

	<p class="text-center">
		<button type="button" class="btn btn-default" id="loadMoreBlocks">Load More</button>
	</p>

  </div>
</div>
<script>
var block,
	Difficulties = [],
    HashRate = [],
	MinFees = [],
	Blocks = [],
	Rewards = [],
	Txses = [],
	Sizes = [],
	Timestamps = [],
	time_stamps = [],
	Solvetimes = [],
	diffChart,
	refresh = true;

currentPage = {
        destroy: function(){
            if (xhrGetBlocks) xhrGetBlocks.abort();
        },
        init: function(){
			$.when(
				renderInitialBlocks()
			).then(function() {
				setTimeout(function(){
					$.when(
						displayCharts()
					).then(function() {
						setTimeout(function(){
							refreshChart();
						}, 100)
					});
				}, 500)
			});
        },
        update: function(){
			renderLastBlock();
            updateText('networkHeight', lastStats.result.top_known_block_height.toString());
            //updateText('networkTransactions', sync_blocks.result.blocks[0].header.already_generated_transactions.toString());
            updateText('networkHashrate', getReadableHashRateString(lastStats.result.top_block_difficulty / blockTargetInterval));
            updateText('networkDifficulty', getReadableDifficultyString(lastStats.result.top_block_difficulty, 2).toString());
			updateText('minimumFee', getReadableCoins(lastStats.result.recommended_fee_per_byte,6));
			$("time.timeago").timeago();
			getPoolTransactions();
			var currHeight = $('#blocks_rows').children().first().data('height');
			if(refresh){
				$.when(
					renderInitialBlocks()
				).then(function() {
					setTimeout(function(){
						refreshChart();
					}, 100)
				});
			}
			if((currHeight + 31) < lastStats.result.last_known_block_index){
				$('#next-page').removeClass('disabled');
			}
		}
};

function renderLastBlock(){
$.ajax({
    url: api + '/json_rpc',
    method: "POST",
    data: JSON.stringify({
          jsonrpc:"2.0",
          id: "test",
          method:"getlastblockheader",
          params: {

                }
    }),
    dataType: 'json',
    cache: 'false',
    success: function(data){
		last_block_hash = data.result.block_header.hash;

        updateText('totalCoins', getReadableCoins(data.result.block_header.already_generated_coins,2));
        updateText('emissionPercent', (data.result.block_header.already_generated_coins / 18446744073709500000 * 100).toFixed(4) + "%");
        updateText('currentReward', getReadableCoins(data.result.block_header.base_reward,4));

        updateText('networkTransactions', data.result.block_header.already_generated_transactions.toString());

        block = data.block_header;

		/*$.ajax({
			url: api + '/json_rpc',
			method: "POST",
			data: JSON.stringify({
				jsonrpc:"2.0",
				id: "test",
				method:"f_block_json",
				params: {
				    hash: last_block_hash
				}
			}),
			dataType: 'json',
			cache: 'false',
			success: function(data){
			block = data.result.block;
				updateText('totalCoins', getReadableCoins(block.alreadyGeneratedCoins,2));
				updateText('emissionPercent', (block.alreadyGeneratedCoins / 10000000000000000000 * 100).toFixed(4) + "%");
				updateText('currentReward', getReadableCoins(block.baseReward,4));
			}
		});*/
    }
});
}

var load_count = 10;
var xhrGetBlocks;

    function loadMore()
    {
        if (xhrGetBlocks) xhrGetBlocks.abort();
        xhrGetBlocks = $.ajax({
            url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "test",
                method:"getblockheaderbyheight",
                params: {
                    height: $('#blocks_rows').children().last().data('height')
                }
            }),
            dataType: 'json',
            cache: 'false',
            success: function(data){
				$.when(
					 renderBlocks(data.result.block_header)
				).then(function() {
                    if(load_count>0){
                        load_count--;
                        loadMore();
                    }else{
                        load_count = 10;
                        setTimeout(function(){
						loadMoreChart();
                        refreshChart();
                        }, 100)
                    }
				});
            }
    });
    }

$('#loadMoreBlocks').click(function(){
    loadMore();
});

$('#prev-page').click(function(e){
	refresh = false;
    if (xhrGetBlocks) xhrGetBlocks.abort();
	var currHeight = $('#blocks_rows').children().first().data('height');
	var openHeight = (currHeight - 31 < 0) ? 0 : currHeight - 31;

	xhrGetBlocks = $.ajax({
            url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "test",
                method:"getblockheaderbyheight",
                params: {
                    height: openHeight
                }
            }),
            dataType: 'json',
            cache: 'false',
            success: function(data){
				$('#blocks_rows').children().remove();
				$.when(
					renderBlocks(data.result.block_header)
				).then(function() {
					setTimeout(function(){
						refreshChart();
					}, 100)
				});
            }
    });
	e.preventDefault();

	if((currHeight + 31) > lastStats.result.last_known_block_index){
		$('#next-page').removeClass('disabled');
	}
	if((currHeight - 61) < 0){
		$('#prev-page').addClass('disabled');
	}
});

$('#next-page').click(function(e){
	refresh = false;
    if (xhrGetBlocks) xhrGetBlocks.abort();
	var currHeight = $('#blocks_rows').children().first().data('height');
	var openHeight = (currHeight + 31 > lastStats.result.last_known_block_index) ? (lastStats.result.last_known_block_index) : currHeight + 31;

	xhrGetBlocks = $.ajax({
            url: api + '/json_rpc',
            method: "POST",
            data: JSON.stringify({
                jsonrpc:"2.0",
                id: "test",
                method:"getblockheaderbyheight",
                params: {
                    height: openHeight
                }
            }),
            dataType: 'json',
            cache: 'false',
            success: function(data){
				$('#blocks_rows').children().remove();
				$.when(
					renderBlocks(data.result.block_header)
				).then(function() {
					setTimeout(function(){
						refreshChart();
					}, 100)
				});
            }
    });
	e.preventDefault();

	if((currHeight + 61) > lastStats.result.last_known_block_index){
		$('#next-page').addClass('disabled');
		refresh = true;
	}

		$('#prev-page').removeClass('disabled');

});

$('#goto-height-go').click(function() {
	var height = document.getElementById('goto-height').value;
	var newUrl = "/?height="+height;
	window.location = newUrl;
});

$('#goto-height').keyup(function(e){
        if(e.keyCode === 13)
            $('#goto-height-go').click();
});

    var count = 30;
    var lastHeigh = 0;

    function renderInitialBlocks(){
        if (xhrGetBlocks) xhrGetBlocks.abort();

		var loadHeight;

		if (urlParam('height'))
            {
				loadHeight = parseInt(urlParam('height'));
                console.log("urlParam loadHeight ")
            }
            else
                {
				//loadHeight = lastStats.result.top_known_block_height - 1;
                loadHeight = lastStats.result.top_known_block_height - count;
                }

		if((loadHeight - 31) < 0){
			$('#prev-page').addClass('disabled');
			$('#next-page').removeClass('disabled');
		}

        if(lastHeigh != 0)
            loadHeight = lastHeigh + 1;

			xhrGetBlocks = $.ajax({
				url: api + '/json_rpc',
				method: "POST",
				data: JSON.stringify({
					jsonrpc:"2.0",
					id: "test",
					method:"getblockheaderbyheight",
					params: {
						height: loadHeight
					}
				}),
				dataType: 'json',
				cache: 'false',
				success: function(data){
                    if(data.result.block_header)
					   renderBlocks(data.result.block_header);
                    if(count >0)
                    {
                        count--;
                        renderInitialBlocks();
                    }
                    else
                    {
                        refreshChart();
                        lastHeigh = loadHeight;
                        if(lastHeigh<lastStats.result.top_known_block_height &&
                           (lastStats.result.top_known_block_height - lastHeigh) < 60 )
                           renderInitialBlocks();
                    }
				}
			});
    };

    function getBlockRowElement(block, jsonString){

        var row = document.createElement('tr');
        row.setAttribute('data-json', jsonString);
        row.setAttribute('data-height', block.height);
        row.setAttribute('id', 'blockRow' + block.height);
        row.setAttribute('title', block.hash);
		var dateTime = new Date(block.timestamp_median * 1000).toISOString();
        //var dateTime = block.timestamp;
		row.setAttribute('data-dt', dateTime);
        row.setAttribute('data-timestamp', block.timestamp_median);
		row.setAttribute('data-min-fee', parseFloat(getReadableCoins(block.total_fee_amount,6,false)));

        var columns =
            '<td>' + block.height + '</td>' +
            '<td class="date-time">' + formatDate(block.timestamp_median) + ' (<time class="timeago" datetime="'+dateTime+'"></time>)</td>' +
			'<td>' + formatBytes(parseInt(block.block_size)) + '</td>' +
			'<td>' + formatBlockLink(block.hash) + '</td>' +
            '<td class="blk-diff">' + block.difficulty + '</td>' +
            '<td>' + block.already_generated_transactions + '</td>';

		Difficulties.push(parseInt(block.difficulty));
        HashRate.push(parseInt(block.difficulty)/*/blockTargetInterval/1000/1000*/);
		MinFees.push(parseFloat(getReadableCoins(block.total_fee_amount,6,false)));
		Blocks.push(parseInt(block.height));

		Txses.push(parseInt(block.already_generated_transactions));
		Sizes.push(parseInt(block.block_size));
		Timestamps.push(dateTime);
		time_stamps.push(block.timestamp_median);

        row.innerHTML = columns;

        return row;
    }

    function renderBlocks(blocksResults){

        var $blocksRows = $('#blocks_rows');

        //for (var i = 0; i < 1; i ++){

            var block = blocksResults;

            var blockJson = JSON.stringify(block);

            var existingRow = document.getElementById('blockRow' + block.height);

            if (existingRow && existingRow.getAttribute('data-json') !== blockJson){
                $(existingRow).replaceWith(getBlockRowElement(block, blockJson));
            }
            else if (!existingRow){

                var blockElement = getBlockRowElement(block, blockJson);

                var inserted = false;
                var rows = $blocksRows.children().get();
                for (var f = 0; f < rows.length; f++) {
                    var bHeight = parseInt(rows[f].getAttribute('data-height'));
                    if (bHeight < block.height){
                        inserted = true;
                        $(rows[f]).before(blockElement);
                        break;
                    }
                }
                if (!inserted) {
                    $blocksRows.append(blockElement);
				}
            }
        //}//for

		$("time.timeago").timeago();
		calcAvgHashRate();
		calcAvgSolveTime();
    }

	function calcAvgHashRate(){
		var sum = Difficulties.reduce(add, 0);
		function add(a, b) {
			return a + b;
		}
		var avgDiff = Math.round(sum / Difficulties.length);
		var avgHashRate = avgDiff / blockTargetInterval;

		updateText('avgDifficulty', getReadableDifficultyString(avgDiff, 2).toString());
		updateText('avgHashrate', getReadableHashRateString(avgDiff / blockTargetInterval));
        updateText('blockSolveTime', getReadableTime(lastStats.result.top_block_difficulty / avgHashRate));

	}

	function calcAvgSolveTime(){
		ts = time_stamps.concat([]);
		ts.sort();
		var avg_solve_time = 0;
		var solveTime = 0;
		for (var i = 1; i < ts.length; i++){
			solveTime += ts[i] - ts[i - 1];
		}
		avg_solve_time = solveTime / (ts.length - 1);
		updateText('avgSolveTime', getReadableTime(avg_solve_time));
	}

// MemPool Transactions
function getPoolTransactions(){
  var xhrGetPool = $.ajax({
	url: api + '/json_rpc',
	method: "POST",
	data: JSON.stringify({
		jsonrpc: "2.0",
		id: "",
		method: "sync_mem_pool",
		params: {}
	}),
	dataType: 'json',
	cache: 'false',
	success: function(newdata) {
		if(typeof newdata.result !== "undefined") {
			var data = newdata.result.added_transactions;
            var status = newdata.result.status;
			var totalAmount = 0;
			var totalFee = 0;
			var totalSize = 0;
			var txcount = 0;
            var mempool_tx_vout_amount = 0;
			var txsRows = document.getElementById('mem_pool_rows');
			  if (txsRows) {
				while (txsRows.firstChild) {
				  txsRows.removeChild(txsRows.firstChild);
				}
			  }
			for (var i = 0; i < data.length; i ++){
				var tx = data[i];

                for(var j=0; j<newdata.result.added_raw_transactions[i].vout.length; j++){
                    mempool_tx_vout_amount = mempool_tx_vout_amount + newdata.result.added_raw_transactions[i].vout[j].amount;
                }
				totalAmount = totalAmount + mempool_tx_vout_amount;
				var row = document.createElement('tr');
				var columns =
					//'<td>' + formatDate(status.top_block_timestamp) + ' (<span class="mtx-ago"></span>)' + '</td>' +
                    '<td>' + formatDate(tx.timestamp) + ' (<span class="mtx-ago"></span>)' + '</td>' +
					'<td>' + getReadableCoins(mempool_tx_vout_amount, 4, true) + '</td>' +
					'<td>' + getReadableCoins(tx.fee, 4, true) + '</td>' +
					'<td>' + formatBytes(parseInt(tx.binary_size)) + '</td>' +
					'<td>' + formatPaymentLink(tx.hash) + '</td>';
                mempool_tx_vout_amount = 0;
				row.innerHTML = columns;
				$(txsRows).append(row);
				//$(row).find('.mtx-ago').timeago('update', new Date(status.top_block_timestamp * 1000).toISOString());
                $(row).find('.mtx-ago').timeago('update', new Date(tx.timestamp * 1000).toISOString());

				txcount = txcount + 1;
				//totalAmount = tx.amount + totalAmount;
				totalFee = totalFee + tx.fee;
				totalSize = totalSize + tx.binary_size;

			}
			updateText('mempool_count', txcount);
			updateText('mempool_amount', getReadableCoins(totalAmount,4));
			updateText('mempool_fees', getReadableCoins(totalFee, 8));
			updateText('mempool_sizes', formatBytes(parseInt(totalSize)));
		}
	}
  });
}

// Difficulty chart
function displayCharts() {
	var ctx = document.getElementById("difficultyChart");
	var chartData = {
			labels: [].concat(Blocks),
			//labels: [].concat(formatDate(Timestamps)).reverse(),
			datasets: [
			  {
				data: [].concat(formatDate(Timestamps)),
				yAxisID: "Time",
				label: "Time",
				backgroundColor: "rgba(0,0,0,0) ",
				borderColor: 'rgba(0,0,0,0)',
				borderWidth: 0,
				pointBorderWidth: 0,
				pointRadius: 0,
				pointHoverRadius: 0,
				pointHitRadius: 0,
				display: false,
				type: 'line'
			  },{
				data: [].concat(HashRate),
				yAxisID: 'Difficulty',//"Hash Rate",
				label: 'Difficulty',//"Hash Rate",
				backgroundColor: "rgba(16,40,63,0.1) ",
				borderColor: '#2FA4E7',
				borderWidth: 2,
				pointColor : "#2FA4E7",
				pointBorderColor : "#2FA4E7",
				pointHighlightFill: "#2FA4E7",
				pointBackgroundColor: "#2FA4E7",
				pointBorderWidth: 2,
				pointRadius: 1,
				pointHoverRadius: 3,
				pointHitRadius: 20,
				type: 'line'
			  },{
				data: [].concat(MinFees),
				yAxisID: "MinFee",
				label: "Tx fee",
				backgroundColor: "rgba(237,28,36,0) ",
				borderColor: '#ED1C24',
				borderWidth: 1,
				pointColor : "#ED1C24",
				pointBorderColor : "#ED1C24",
				pointHighlightFill: "#ED1C24",
				pointBackgroundColor: "#ED1C24",
				pointBorderWidth: 2,
				pointRadius: 1,
				pointHoverRadius: 3,
				pointHitRadius: 20,
				type: 'line'
			  },{
				data: [].concat(Txses),
				yAxisID: "Transactions",
				label: "Transactions",
				backgroundColor: "rgba(0,0,0,0) ",
				borderColor: 'rgba(0,0,0,0)',
				borderWidth: 0,
				type: 'line'
			  },{
				data: [].concat(Sizes),
				yAxisID: "Sizes",
				label: "Size",
				backgroundColor: "rgba(65,172,0,0.4) ",
				borderColor: '#99D9EA',
				borderWidth: 0,
				type: 'bar'
			  },{
				data: [].concat(Blocks),
				yAxisID: "Height",
				label: "Height",
				backgroundColor: "rgba(0,0,0,0) ",
				borderColor: 'rgba(0,0,0,0)',
				borderWidth: 0,
				pointBorderWidth: 0,
				pointRadius: 0,
				pointHoverRadius: 0,
				pointHitRadius: 0,
				display: false,
				type: 'line'
			  }
			]
		};

	var options = {
			responsive: true,
			maintainAspectRatio: false,
			elements: {
				line: {
					tension: 0
				}
			},
			title: {
				display: false
			},
			legend: {
				display: false
			},
			scales: {
			  yAxes: [{
				id: 'Time',
				type: 'linear',
				position: 'left',
				scaleLabel: {
					display: false,
					labelString: 'Time'
				},
				gridLines: {
					display:false
				},
				ticks: {
					fontSize: 9,
					display:false
				},
				display: true
			  },{
				id: 'Difficulty',//'Hash Rate',
				type: 'linear',

				position: 'left',
				scaleLabel: {
					display: true,
					labelString: 'Difficulty'//'Hash Rate'
				},
				gridLines: {
					display:true
				},
				ticks: {
					fontSize: 9,
					display:true/*,
                    userCallback: function(value, index, values) {
                    // Convert the number to a string and splite the string every 3 charaters from the end
                    value = value.toString();
                    value = value.split(/(?=(?:...)*$)/);
                    // Convert the array to a string and format the output
                    value = value.join('.');
                    return value + " MHs";
                    }*/
				},
				display: true
			  },{
				id: 'Transactions',
				type: 'linear',
				position: 'right',
				scaleLabel: {
					display: true,
					labelString: 'Transactions'
				},
				gridLines: {
					display:false
				},
				ticks: {
					fontSize: 9,
					display:true
				},
				display: false
			  },{
				id: 'Sizes',
				type: 'linear',
				position: 'right',
				barThickness: 10,
				scaleLabel: {
					display: true,
					labelString: 'Size'
				},
				gridLines: {
					display:false
				},
				ticks: {
					fontSize: 9,
					display:true
				},
				display: true
			  },{
				id: 'MinFee',
				type: 'linear',
				position: 'right',
				scaleLabel: {
					display: true,
					labelString: 'Tx Fee'
				},
				gridLines: {
					display:false
				},
				ticks: {
					fontSize: 9,
					display:true,
					suggestedMin: 0.001,
					suggestedMax: 0.01
				},
				display: true
			  },{
				id: 'Height',
				type: 'linear',
				position: 'right',
				scaleLabel: {
					display: true,
					labelString: 'Height'
				},
				gridLines: {
					display:false
				},
				ticks: {
					fontSize: 9,
					display:true,
					suggestedMin: 0.001,
					suggestedMax: 0.01
				},
				display: false
			  }

			  ],
			  xAxes: [{


                distribution: 'series',
				categoryPercentage: 0,
				barPercentage: 1,
				categorySpacing: 0,
				barThickness: 10,

				/*time: {
                    parser: false,
					unit: 'minute',
					unitStepSize: 60,
					round: 'second',
					displayFormats: {
                                        'millisecond': 'SSS [ms]',
                                        'second': 'HH:mm:ss', // 11:20:01 AM
                                        'minute': 'HH:mm', // 11:20:01 AM
                                        'hour': 'HH:mm', // Sept 4, 5PM
                                        'day': 'MMM Do', // Sep 4 2015
                                        'week': 'll', // Week 46, or maybe "[W]WW - YYYY" ?
                                        'month': 'MMM YYYY', // Sept 2015
                                        'quarter': '[Q]Q - YYYY', // Q3
                                        'year': 'YYYY', // 2017
                                    },
				},*/
				gridLines: {
					display:true
				},
                scaleLabel: {
					display: true,
					labelString: 'Height',
                    fontColor: "#CCC",
                    fontSize: 9
				},
				ticks: {
					fontSize: 8,
					fontColor: "#CCC",
					autoSkip: true,
					//maxRotation: 90,
					//minRotation: 90
					display:true,
                    autoSkipPadding: 1
				}
				//display: false
			  }]
			},
			tooltips: {
				mode: 'index',
				intersect: true,
				//displayColors: false,
				callbacks: {
					title: function(tooltipItem, data) {
						var height	= data.datasets[3].data[tooltipItem[0].index];
						//var time = new Date(data.labels[tooltipItem[0].index]).toLocaleString();
                        var time = new Date(data.datasets[0].data[tooltipItem[0].index]).toLocaleString();
						//return 'Block: ' + height + ' - ' + time + '';
						return time + '';
                        //return time;
					}/*,
					label: function(tooltipItem, data) {
						var diff		= data.datasets[0].data[tooltipItem.index];
						var txcount	= data.datasets[1].data[tooltipItem.index];
						var size		= data.datasets[2].data[tooltipItem.index];

						var diff_swatch_color = data.datasets[0].borderColor;

						console.log(diff_swatch_color);

						var diff_swatch = '<span style="color="'+ diff_swatch_color +'">&bullet;</span>';

						var multistringText = [];

						var str1 = diff_swatch + 'Difficulty: ' + diff + '';
						var str2 = 'Size: ' + size + '';
						var str3 = 'Transactions: ' + txcount + '';

						multistringText.push(str1);
						multistringText.push(str2);
						multistringText.push(str3);

						return multistringText;
					}*/
				}
			  }
		};

	diffChart = new Chart(ctx,{
    type: 'bar',
    data: chartData,
    options: options
	});
}

function mysort(t){
    if(!t || !t.length || t.length ==0)
        return[[],[]];
	const max = Number.POSITIVE_INFINITY;
	const min = Number.NEGATIVE_INFINITY;

	var min_value = max;
	var max_value = min;
	var s = [];
	var index = [];

	for(var j in t){
		for(var i in t){
			if(parseInt(t[i])>max_value && parseInt(t[i])<min_value)
				max_value = parseInt(t[i]);
		}

		for(var repeted in t){
			if(max_value == parseInt(t[repeted])){
				s.push(max_value);
				index.push(parseInt(repeted));
			}
		}
		min_value = max_value;
		max_value = min;
	}
	if(t.length != s.length)
		s.splice(t.length,s.length-t.length);
	return [s,index];
}

function refreshChart() {
	var brows   = $('#blocks_rows').children(),
		labels  = [],
		diffs   = [],
		sizes   = [],
		times   = [],
		heights = [],
		txsnr   = [];
		mfees   = [];
		for (var i = 0; i < brows.length; i++) {
			var row = $(brows[i]);
			var label = row.data("json").height;
			var diff  = row.data("json").difficulty/*/blockTargetInterval/1000/1000*/;
			var txses = row.data("json").already_generated_transactions;
			var size  = row.data("json").block_size;
			//var time  = row.attr('data-dt');
            var time  = row.attr('data-timestamp');
			var mfee  = row.attr('data-min-fee');

				labels.push(label);
				diffs.push(diff);

				txsnr.push(txses);

				sizes.push(size);
				times.push(time);
				mfees.push(mfee);
		}

//=================================================================================================
    var order = mysort(labels)[1];

    order = order.reverse();

    var _labels = [];
    var _diffs = [];
    var _mfees = [];
    var _txsnr = [];
    var _sizes = [];
    var _times = [];

    for (var i in order){
        var dateTime = new Date( times[parseInt(order[i])] * 1000).toISOString();
        _times.push(dateTime);
		_labels.push(labels[parseInt(order[i])]);
        _diffs.push(diffs[parseInt(order[i])]);
        _mfees.push(mfees[parseInt(order[i])]);
        _txsnr.push(txsnr[parseInt(order[i])]);
        _sizes.push(sizes[parseInt(order[i])]);
    }

    diffChart.data.labels = _labels;//_times.reverse();
    diffChart.data.datasets[0].data = _times;//_labels.reverse();
    diffChart.data.datasets[1].data = _diffs;
    diffChart.data.datasets[2].data = _mfees;
    diffChart.data.datasets[3].data = _txsnr;
    diffChart.data.datasets[4].data = _sizes;
    diffChart.data.datasets[5].data = _labels;
//=================================================================================================

    diffChart.update();
}

function loadMoreChart() {
	diffChart.data.labels = [].concat(Blocks);
	//diffChart.data.labels = [].concat(Timestamps).reverse();
	diffChart.data.datasets[0].data = [].concat(Timestamps);//[].concat(Blocks).reverse();
	diffChart.data.datasets[1].data = [].concat(HashRate);
	diffChart.data.datasets[2].data = [].concat(MinFees);
	diffChart.data.datasets[3].data = [].concat(Txses);
	diffChart.data.datasets[4].data = [].concat(Sizes);
    diffChart.data.datasets[5].data = [].concat(Blocks);

	diffChart.update();
}

$(function() {
    $('[data-toggle="tooltip"]').tooltip();
});

function getReadableTime(seconds){

        var units = [ [60, 's.'], [60, 'min.'], [24, 'h.'],
            [7, 'd.'], [4, 'w.'], [12, 'ĐĽ.'], [1, 'y.'] ];

        function formatAmounts(amount, unit){
            var rounded = Math.round(amount);
            return '' + rounded + ' ' + unit + (rounded > 1 ? '' : '');
        }

        var amount = seconds;
        for (var i = 0; i < units.length; i++){
            if (amount < units[i][0])
                return formatAmounts(amount, units[i][1]);
            amount = amount / units[i][0];
        }
        return formatAmounts(amount,  units[units.length - 1][1]);
}

</script>

<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/2.8.0/Chart.bundle.min.js"></script>

)";

static const std::string block_html =
    R"(
<h2><i class="fa fa-cube fa-fw" aria-hidden="true"></i> Block <small id="block.hash" style="word-break: break-all;"></small></h2>
<div class="row">
    <div class="col-md-6 stats">
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="Block index in the chain, counting from zero (i.e. genesis block)."><i class="fa fa-question-circle"></i></span> Height: <span id="block_height"><span id="block.height"></span></span></div>
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="Block timestamp displayed as UTC. The timestamp correctness it up to miner, who mined the block."><i class="fa fa-question-circle"></i></span> Timestamp: <span id="block.timestamp"></span></div>
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="Block timestamp median displayed as UTC."><i class="fa fa-question-circle"></i></span> Timestamp median: <span id="block.timestamp_median"></span></div>
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="â€śmajor versionâ€ť.â€śminor versionâ€ť"><i class="fa fa-question-circle"></i></span> Version: <span id="block.version"></span></div>
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="How difficult it is to find a solution for the block. More specifically, it`s mathematical expectation for number of hashes someone needs to calculate in order to find a correct nonce value solving the block."><i class="fa fa-question-circle"></i></span> Difficulty: <span id="block.difficulty"></span></div>
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="True, if the block belongs to an alternative chain. In such case all the transactions, excluding coinbase, are removed from the block back to transaction pool to be included in another block. It means there is no reward for the miner."><i class="fa fa-question-circle"></i></span> Orphan: <span id="block.orphan"></span></div>
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="Number of transactions in the block, including coinbase transaction (which transfers block reward to the miner)."><i class="fa fa-question-circle"></i></span> Transactions: <span id="block.transactions"></span></div>
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="Cumulative amount of coins issued by all the blocks in blockchain from the genesis and up to this block."><i class="fa fa-question-circle"></i></span> Total coins in the network: <span id="block.totalCoins"></span></div>
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="Cumulative number of transactions in the blockchain, from the genesis block and up to this block."><i class="fa fa-question-circle"></i></span> Total transactions in the network: <span id="block.totalTransactions"></span></div>
    </div>

    <div class="col-md-6 stats">
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="Cumulative size of all transactions in the block, including coinbase. In case it's exceeding 'effective txs median' the reward penalty occurs and therefore miner receives less reward."><i class="fa fa-question-circle"></i></span> Total transactions size, bytes: <span id="block.transactionsSize"></span></div>
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="Size of the whole block, i.e. block header plus all transactions."><i class="fa fa-question-circle"></i></span> Total block size, bytes: <span id="block.blockSize"></span></div>
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="Median value of block total transactions size among last n blocks."><i class="fa fa-question-circle"></i></span> Current txs median, bytes: <span id="block.currentTxsMedian"></span></div>
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="Bounded from below median value that is actually used to calculate penalty. More specifically, &lt;effective median&gt; = max(&lt;current median&gt;, 20000) "><i class="fa fa-question-circle"></i></span> Effective txs median, bytes: <span id="block.effectiveTxsMedian"></span></div>
        <!-- <div><span data-toggle="tooltip" data-placement="right" data-original-title="Penalty for exceeding the median. &lt;penalty&gt; = (&lt;total&nbsp;transactions&nbsp;size&gt; / &lt;effective&nbsp;tx&nbsp;median&gt; â’ 1) ^ 2. No penalty if total transactions size is less then effective median. Penalty is near 100% if total txs size is twice the effective median. Greater blocks are not allowed."><i class="fa fa-question-circle"></i></span> Reward penalty: <span id="block.rewardPenalty"></span></div> -->
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="Base value for calculating the block reward. Does not depend on how many transactions are included into the block. Also, this is how many coins the miner would receive if the block contains only coinbase transaction."><i class="fa fa-question-circle"></i></span> Base reward: <span id="block.baseReward"></span></div>
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="Sum of fees for all transactions in the block."><i class="fa fa-question-circle"></i></span> Transactions fee: <span id="block.transactionsFee"></span></div>
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="Actual amount of coins the miner received for finding the block. &lt;reward&gt; = &lt;base reward&gt; Ă— (1 â’ &lt;penalty&gt;) + &lt;transactions fee&gt;"><i class="fa fa-question-circle"></i></span> Reward: <span id="block.reward"></span></div>
    </div>
</div>

<h3 class="transactions"><i class="fa fa-exchange fa-fw" aria-hidden="true"></i> Transactions</h3>
<div class="table-responsive">
    <table class="table table-hover">
        <thead>
        <tr>
            <th><i class="fa fa-paw"></i> Hash</th>
            <th><i class="fa fa-percent"></i> Fee</th>
            <th><i class="fa fa-money"></i> Total Amount</th>
            <th><i class="fa fa-arrows"></i> Size</th>
        </tr>
        </thead>
        <tbody id="transactions_rows">

        </tbody>
    </table>
</div>

<script>
    var block, xhrGetBlock;

    currentPage = {
        destroy: function(){
            if (xhrGetBlock) xhrGetBlock.abort();
        },
        init: function(){
            getBlock();
        },
        update: function(){
        }
    };

    function getBlock(){
        if (xhrGetBlock) xhrGetBlock.abort();
		var searchBlk = $.parseJSON(sessionStorage.getItem('searchBlock'));
		if (searchBlk) {
			renderBlock(searchBlk);
		} else {
			xhrGetBlock = $.ajax({
				url: api + '/json_rpc',
				method: "POST",
				data: JSON.stringify({
					jsonrpc:"2.0",
					id: "test",
					method:"get_raw_block",
					params: {
						hash: window.location.hash.substring(1)
					}
				}),
				dataType: 'json',
				cache: 'false',
				success: function(data){
					block = data.result;
					renderBlock(block);
				}
			});
		}
		sessionStorage.removeItem('searchBlock');
    }

	function renderBlock(block){
				updateText('block.hash', block.header.hash);
				updateText('block.height', block.header.height);
				updateText('block.timestamp', formatDate(block.header.timestamp));
                updateText('block.timestamp_median', formatDate(block.header.timestamp_median));
				updateText('block.version', block.header.major_version + '.' + block.header.minor_version);
				updateText('block.difficulty', block.header.difficulty);
				//updateText('block.orphan', block.orphan_status ? "Orphan" : "Valid Block");
				//updateText('block.transactions', block.transactions.length);
				updateText('block.transactionsSize', formatBytes(parseInt(block.header.transactions_cumulative_size)));
				updateText('block.blockSize', formatBytes(parseInt(block.header.block_size)));
				updateText('block.currentTxsMedian', formatBytes(parseInt(block.header.size_median)));
				updateText('block.effectiveTxsMedian', formatBytes(parseInt(block.header.effective_size_median)));
				//updateText('block.rewardPenalty', block.penalty*100 + "%");
				updateText('block.baseReward', getReadableCoins(block.header.base_reward));
				updateText('block.transactionsFee', getReadableCoins(block.header.total_fee_amount));
				updateText('block.reward', getReadableCoins(block.header.reward));
				updateText('block.totalCoins', getReadableCoins(block.header.already_generated_coins));
				updateText('block.totalTransactions', block.header.already_generated_transactions);
                updateText('block.transactions', block.transaction_binary_sizes.length);
				renderTransactions(block);

				makePrevBlockLink(block.header.prev_hash);

				$.ajax({
					url: api + '/json_rpc',
					method: "POST",
					data: JSON.stringify({
					jsonrpc: "2.0",
					id: "test",
					method: "getblockheaderbyheight",
					params: {
						height: (block.header.height + 2)
					}
					}),
					dataType: 'json',
					cache: 'false',
					success: function(data){
					  if(data.result){
						var nextBlockHash = data.result.block_header.hash;
					  }
						 if(nextBlockHash) {
							makeNextBlockLink(nextBlockHash);
						 }
					},
					error: function (ajaxContext) {
					}
				});


                $.ajax({
					url: api + '/json_rpc',
					method: "POST",
					data: JSON.stringify({
					jsonrpc: "2.0",
					id: "test",
					method: "getblockheaderbyhash",
					params: {
						hash: block.header.hash
					}
					}),
					dataType: 'json',
					cache: 'false',
					success: function(data){
                        updateText('block.orphan', data.result.block_header.orphan_status ? "Orphan" : "Valid Block");
					},
					error: function (ajaxContext) {
					}
				});

	}

    function getTransactionCells(transaction){
        return '<td>' + formatPaymentLink(transaction.hash) + '</td>' +
               '<td>' + getReadableCoins(transaction.fee, 4, true) + '</td>' +
               '<td>' + getReadableCoins(transaction.amount_out, 4, true) + '</td>' +
               '<td>' + formatBytes(parseInt(transaction.size)) + '</td>';
    }

    function getTransactionRowElement(tx, jsonString){

        var row = document.createElement('tr');
        row.setAttribute('data-json', jsonString);
        row.setAttribute('data-hash', tx.hash);
        row.setAttribute('id', 'transactionRow' + tx.hash);

        row.innerHTML = getTransactionCells(tx);

        return row;
    }

    function renderTransactions(block){

        var $transactionsRows = $('#transactions_rows');

        for (var i = 0; i < block.raw_transactions.length; i++){

            var transaction = block.raw_transactions[i];
            var transaction_hash = block.raw_header.tx_hashes[i];

            var transactionJson = JSON.stringify(transaction);

            var existingRow = document.getElementById('transactionRow' + transaction_hash);

            var vout = 0;

            for (var j = 0; j < block.raw_transactions[i].vout.length; j++) {
                    vout = block.raw_transactions[i].vout[j].amount + vout;
            }

            (function(transaction_hash,vout,block,i){
            xhrGetTransaction = $.ajax({
				url: api + '/json_rpc',
				method: "POST",
				data: JSON.stringify({
					jsonrpc:"2.0",
					id: "test",
					method:"get_raw_transaction",
					params: {
						hash: transaction_hash
					}
				}),
				dataType: 'json',
				cache: 'false',
				success: function(data){
                    var tx =  { hash: transaction_hash, fee: 0, amount_out: vout, size: block.transaction_binary_sizes[i+1] };
                    tx.fee = data.result.transaction.fee;
                    if (existingRow && existingRow.getAttribute('data-json') !== transactionJson){
                        $(existingRow).replaceWith(getTransactionRowElement(tx, transactionJson));
                    }
                    else if (!existingRow){
                        var transactionElement = getTransactionRowElement(tx, transactionJson);
                        $transactionsRows.append(transactionElement);
                    }
				}
			});
            })(transaction_hash,vout,block,i);//passing in variable to var here

        }
    }

	function makeNextBlockLink(blockHash){
		$('#block_height').append(' <a href="' + getBlockchainUrl(blockHash) + '" title="Next block"><i class="fa fa-chevron-circle-right" aria-hidden="true"></i></a>');
	}

	function makePrevBlockLink(blockHash){
		$('#block_height').prepend('<a href="' + getBlockchainUrl(blockHash) + '" title="Previous block"><i class="fa fa-chevron-circle-left" aria-hidden="true"></i></a> ');
	}

	function formatPrevNextBlockLink(hash){
        return '<a href="' + getBlockchainUrl(hash) + '">' + hash + '</a>';
    }

	$(function() {
		$('[data-toggle="tooltip"]').tooltip();
	});

</script>

)";

static const std::string tx_html =
    R"(

<h2><i class="fa fa-exchange fa-fw" aria-hidden="true"></i> Transaction</h2>
<div class="row" id="tx_info">
    <div class="col-md-12 stats">
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="Unique fingerprint of the transaction."><i class="fa fa-question-circle"></i></span> Hash: <span id="transaction.hash" style="word-break: break-all;"></span></div>
		<div id="confirmations" style="display: none;"><span data-toggle="tooltip" data-placement="right" data-original-title="The number of network confirmations."><i class="fa fa-question-circle"></i></span> Confirmations: <span id="transaction.confirmations"></span>, First confirmation time: <span id="transaction.timestamp"></span> (<time class="transaction-timeago"></time>)</div>
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="Money that goes to the miner, who included this transaction into block."><i class="fa fa-question-circle"></i></span> Fee: <span id="transaction.fee"></span></div>
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="It does not mean that this is the amount that is actually transferred."><i class="fa fa-question-circle"></i></span> Sum of outputs: <span id="transaction.amount_out"></span></div>
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="It does not mean that this is the amount that is actually transferred."><i class="fa fa-question-circle"></i></span> Sum of inputs: <span id="transaction.amount_inp"></span></div>
        <div><span data-toggle="tooltip" data-placement="right" data-original-title="Size of the transaction in bytes."><i class="fa fa-question-circle"></i></span> Size: <span id="transaction.size"></span></div>
        <div id="div_transaction_paymentId"><span data-toggle="tooltip" data-placement="right" data-original-title="Optional user-defined hexadecimal characters string. Can be used by anyone to distinguish the transactions easier."><i class="fa fa-question-circle"></i></span> Payment ID: <span id="transaction.paymentId"></span>
		<br />
        Payment ID decoding: <em id="transaction.paymentIdDecifer"></em></div>
        <div id="div_transaction_mixin"><span data-toggle="tooltip" data-placement="right" data-original-title="Denotes how many random inputs are mixed within this transactions in order to achieve desired level of anonimity. Mixin count 1 means no additional inputs are mixed in and thus each input can be traced back."><i class="fa fa-question-circle"></i></span> Mixin count: <span id="transaction.mixin"></span></div>
		<div id="tx_unconfirmed" style="display: none;"><span data-toggle="tooltip" data-placement="right" data-original-title="The transaction is not included into block yet and therefore is not wtitten into blockchain."><i class="fa fa-question-circle"></i></span> <span class="text-warning">Unconfirmed transaction</span></div>
    </div>
</div>
<div id="tx_block">
	<h3><i class="fa fa-cube fa-fw" aria-hidden="true"></i> In block</h3>
	<div class="row">
		<div class="col-md-12 stats">
			<div><i class="fa fa-circle-o"></i> Hash: <span id="block.hash" style="word-break: break-all;"></span></div>
			<div><i class="fa fa-circle-o"></i> Height: <span id="block.height"></span></div>
			<div><i class="fa fa-circle-o"></i> Timestamp: <span id="block.timestamp"></span></div>
		</div>
	</div>
</div>

<h3 class="inputs">Inputs (<span id="inputs_count"></span>)</h3>
<div class="table-responsive">
    <table class="table table-hover">
        <thead>
        <tr>
            <th><i class="fa fa-money"></i> Amount</th>
            <th><i class="fa fa-paw"></i> Image</th>
			<th><i class="fa fa-arrows-alt"></i> Offset</th>
        </tr>
        </thead>
        <tbody id="inputs_rows">

        </tbody>
    </table>
</div>


<h3 class="outputs">Outputs (<span id="outputs_count"></span>)</h3>
<div class="table-responsive">
    <table class="table table-hover">
        <thead>
        <tr>
            <th><i class="fa fa-money"></i> Amount</th>
            <th><i class="fa fa-key"></i> Key</th>
        </tr>
        </thead>
        <tbody id="outputs_rows">

        </tbody>
    </table>
</div>

<script>
    var xhrGetTransaction, transaction;

    currentPage = {
        destroy: function(){
            if (xhrGetTransaction) xhrGetTransaction.abort();
        },
        init: function(){
            getTransaction();
        },
        update: function(){
        }
    };

    function getTransaction(){
        if (xhrGetTransaction) xhrGetTransaction.abort();
		var searchTx = $.parseJSON(sessionStorage.getItem('searchTransaction'));
		if (searchTx) {
			renderTransaction(searchTx);
		} else {
			xhrGetTransaction = $.ajax({
				url: api + '/json_rpc',
				method: "POST",
				data: JSON.stringify({
					jsonrpc:"2.0",
					id: "test",
					method:"get_raw_transaction",
					params: {
						hash: window.location.hash.substring(1)
					}
				}),
				dataType: 'json',
				cache: 'false',
				success: function(data){
					var tx = data.result;
					renderTransaction(tx);
				}
			});
		}
		sessionStorage.removeItem('searchTransaction');
	}

	function renderTransaction(transaction){
				var details = transaction.transaction;
                inputs = transaction.raw_transaction.vin;
                outputs = transaction.raw_transaction.vout;
                //block = transaction.block;

                updateText('transaction.hash', details.hash);
                if (details.block_hash){
					$('#confirmations').show();
					updateText('transaction.confirmations', lastStats.result.top_known_block_height - details.block_height);
					updateText('transaction.timestamp', formatDate(details.timestamp));
                    if(details.timestamp > 0)
					   $(".transaction-timeago").timeago('update', new Date(details.timestamp * 1000).toISOString());
				}
                //updateText('transaction.amount_out', getReadableCoins(details.amount));
                updateText('transaction.fee', getReadableCoins(details.fee));
                updateText('transaction.mixin', details.anonymity);
                if (!details.anonymity)
                    $('#div_transaction_mixin').hide();
                updateText('transaction.paymentId', details.payment_id);
				updateText('transaction.paymentIdDecifer', hex2a(details.payment_id));
                if (!details.payment_id)
                    $('#div_transaction_paymentId').hide();
                updateText('transaction.size', formatBytes(parseInt(details.binary_size)));

				if (!details.block_hash){
					$('#tx_block').hide();
					$('#tx_unconfirmed').show();
				}

                updateTextLinkable('block.hash', formatBlockLink(details.block_hash));
                updateText('block.height', details.block_height);
                updateText('block.timestamp', formatDate(details.timestamp));

                renderInputs(inputs);
                renderOutputs(outputs);
	}

    function getInputCells(input){
        return '<td>' + getReadableCoins(input.amount) + '</td>' +
            '<td>' + input.key_image + '</td>' +
            '<td>' + input.output_indexes + '</td>';
    }


    function getInputRowElement(input, jsonString){

        var row = document.createElement('tr');
        row.setAttribute('data-json', jsonString);
        row.setAttribute('data-k_image', input.key_image);
		row.setAttribute('data-key_offsets', input.output_indexes);
        row.setAttribute('id', 'inputRow' + input.key_image);

        row.innerHTML = getInputCells(input);

        return row;
    }

    function renderInputs(inputResults){

        var $inputsRows = $('#inputs_rows');
        var sum_inputs = 0;

        for (var i = 0; i < inputResults.length; i++){

            var input = inputResults[i];

            if (!input.amount)
                continue;

            var inputJson = JSON.stringify(input);

            var existingRow = document.getElementById('inputRow' + input.key_image);

            if (existingRow && existingRow.getAttribute('data-json') !== inputJson){
                $(existingRow).replaceWith(getInputRowElement(input, inputJson));
            }
            else if (!existingRow){

                var inputElement = getInputRowElement(input, inputJson);
                $inputsRows.append(inputElement);
            }

            sum_inputs += input.amount;

        }

        updateText('transaction.amount_inp', getReadableCoins(sum_inputs));
		updateText('inputs_count', document.querySelectorAll('#inputs_rows tr').length);
    }


    function getOutputCells(output){
        return '<td>' + getReadableCoins(output.amount) + '</td>' +
            '<td>' + output.target.key + '</td>';
    }


    function getOutputRowElement(output, jsonString){

        var row = document.createElement('tr');
        row.setAttribute('data-json', jsonString);
        row.setAttribute('data-k_image', output.target.key);
        row.setAttribute('id', 'outputRow' + output.target.key);

        row.innerHTML = getOutputCells(output);

        return row;
    }

    function renderOutputs(outputResults){

        var $outputsRows = $('#outputs_rows');
        var sum_outputs = 0;

        for (var i = 0; i < outputResults.length; i++){

            var output = outputResults[i];

            var outputJson = JSON.stringify(output);

            var existingRow = document.getElementById('outputRow' + output.target.key);

            if (existingRow && existingRow.getAttribute('data-json') !== outputJson){
                $(existingRow).replaceWith(getOutputRowElement(output, outputJson));
            }
            else if (!existingRow){

                var outputElement = getOutputRowElement(output, outputJson);
                $outputsRows.append(outputElement);
            }

            sum_outputs += output.amount;

        }

        updateText('transaction.amount_out', getReadableCoins(sum_outputs));

		updateText('outputs_count', document.querySelectorAll('#outputs_rows tr').length);
    }

	$(function() {
		$('[data-toggle="tooltip"]').tooltip();
	});
</script>


)";

static const std::string payid_html =
    R"(

<h2><i class="fa fa-tag fa-fw" aria-hidden="true"></i> Payment ID <small id="paymend_id" style="word-break: break-all;"></small></h2>

<h3><i class="fa fa-exchange fa-fw" aria-hidden="true"></i> Transactions with this Payment ID</h3>
<div class="table-responsive">
    <table class="table table-hover">
        <thead>
        <tr>
            <th><i class="fa fa-paw"></i> Hash</th>
            <th><i class="fa fa-percent"></i> Fee</th>
            <th><i class="fa fa-money"></i> Total Amount</th>
            <th><i class="fa fa-arrows"></i> Size</th>
        </tr>
        </thead>
        <tbody id="transactions_rows">

        </tbody>
    </table>
</div>

<script>
    var paymentId, xhrGetTsx, txsByPaymentId;

	paymentId = urlParam('hash');
	updateText('paymend_id', paymentId);

    currentPage = {
        destroy: function(){
            if (xhrGetTsx) xhrGetTsx.abort();
        },
        init: function(){
            getTransactions();
        },
        update: function(){
        }
    };

    function getTransactions(){
        if (xhrGetTsx) xhrGetTsx.abort();
		txsByPaymentId = $.parseJSON(sessionStorage.getItem('txsByPaymentId'));
        if (txsByPaymentId) {
			renderTransactions(txsByPaymentId);
		} else {
			xhrGetTsx = $.ajax({
				url: api + '/json_rpc',
				method: "POST",
				data: JSON.stringify({
					jsonrpc:"2.0",
					id: "test",
					method:"k_transactions_by_payment_id",
					params: {
						payment_id: window.location.hash.substring(1)
					}
				}),
				dataType: 'json',
				cache: 'false',
				success: function(data){
					var txs = data.result.transactions;
					renderTransactions(txs);
				}
			});
		}
		sessionStorage.removeItem('txsByPaymentId');
	}

    function getTransactionCells(transaction){
        return '<td>' + formatPaymentLink(transaction.hash) + '</td>' +
               '<td>' + getReadableCoins(transaction.fee, 4, true) + '</td>' +
               '<td>' + getReadableCoins(transaction.amount_out, 4, true) + '</td>' +
               '<td>' + formatBytes(parseInt(transaction.size)) + '</td>';
    }

    function getTransactionRowElement(transaction, jsonString){

        var row = document.createElement('tr');
        row.setAttribute('data-json', jsonString);
        row.setAttribute('data-hash', transaction.hash);
        row.setAttribute('id', 'transactionRow' + transaction.hash);

        row.innerHTML = getTransactionCells(transaction);

        return row;
    }

    function renderTransactions(transactionResults){

        var $transactionsRows = $('#transactions_rows');

        for (var i = 0; i < transactionResults.length; i++){

            var transaction = transactionResults[i];

            var transactionJson = JSON.stringify(transaction);

            var existingRow = document.getElementById('transactionRow' + transaction.hash);

            if (existingRow && existingRow.getAttribute('data-json') !== transactionJson){
                $(existingRow).replaceWith(getTransactionRowElement(transaction, transactionJson));
            }
            else if (!existingRow){

                var transactionElement = getTransactionRowElement(transaction, transactionJson);
                $transactionsRows.append(transactionElement);
            }

        }
    }

	$(function() {
		$('[data-toggle="tooltip"]').tooltip();
	});

</script>


)";

static const std::string robots_txt             = "User-agent: *\r\nDisallow: /";

bool Node::on_api_http_request(http::Client *who, http::RequestData &&request, http::ResponseData &response) {
	response.r.add_headers_nocache();
    //response.r.headers.push_back({ "Access-Control-Allow-Origin", "*" });
	if (request.r.uri == "/" || request.r.uri == "/index.html") {
		response.r.headers.push_back({"Content-Type", "text/html; charset=UTF-8"});
		response.r.status = 200;
		auto stat         = create_status_response3();
		response.set_body(beautiful_index_start + app_version() + " &bull; sync status " +
		                  common::to_string(stat.top_block_height) + "/" +
		                  common::to_string(stat.top_known_block_height) + beautiful_index_finish);
		return true;
	}

	if (request.r.uri == "/config.js") {
		response.r.headers.push_back({"Content-Type", "text/javascript; charset=UTF-8"});
		response.r.status = 200;
		response.set_body(std::string(config_js));
		return true;
	}
	if (request.r.uri == "/js/cookie.js") {
		response.r.headers.push_back({"Content-Type", "text/javascript; charset=UTF-8"});
		response.r.status = 200;
		response.set_body(std::string(cookie_js));
		return true;
	}
	if (request.r.uri == "/js/sorttable.js") {
		response.r.headers.push_back({"Content-Type", "text/javascript; charset=UTF-8"});
		response.r.status = 200;
		response.set_body(std::string(sorttable_js));
		return true;
	}
	if (request.r.uri == "/css/themes/bootstrap.min.css") {
		response.r.headers.push_back({"Content-Type", "text/css; charset=UTF-8"});
		response.r.status = 200;
		response.set_body(std::string(bootstrap_min_css));
		return true;
	}
	if (request.r.uri == "/css/themes/dark-theme.css") {
		response.r.headers.push_back({"Content-Type", "text/css; charset=UTF-8"});
		response.r.status = 200;
		response.set_body(std::string(dark_theme_css));
		return true;
	}
	if (request.r.uri == "/css/themes/default-theme.css") {
		response.r.headers.push_back({"Content-Type", "text/css; charset=UTF-8"});
		response.r.status = 200;
		response.set_body(std::string(default_theme_css));
		return true;
	}
	if (request.r.uri == "/css/themes/white-theme.css") {
		response.r.headers.push_back({"Content-Type", "text/css; charset=UTF-8"});
		response.r.status = 200;
		response.set_body(std::string(white_theme_css));
		return true;
	}
	if (request.r.uri == "/css/themes/dark/style.css") {
		response.r.headers.push_back({"Content-Type", "text/css; charset=UTF-8"});
		response.r.status = 200;
		response.set_body(std::string(style_css));
		return true;
	}
	if (request.r.uri == "/css/themes/dark/bootstrap.min.css") {
		response.r.headers.push_back({"Content-Type", "text/css; charset=UTF-8"});
		response.r.status = 200;
		response.set_body(std::string(dark_bootstrap_min_css));
		return true;
	}
	if (request.r.uri == "/pages/home.html") {
		response.r.headers.push_back({"Content-Type", "text/html; charset=UTF-8"});
		response.r.status = 200;
		response.set_body(std::string(home_html));
		return true;
	}
	if (request.r.uri == "/pages/blockchain_block.html") {
		response.r.headers.push_back({"Content-Type", "text/html; charset=UTF-8"});
		response.r.status = 200;
		response.set_body(std::string(block_html));
		return true;
	}
	if (request.r.uri == "/pages/blockchain_transaction.html") {
		response.r.headers.push_back({"Content-Type", "text/html; charset=UTF-8"});
		response.r.status = 200;
		response.set_body(std::string(tx_html));
		return true;
	}
	if (request.r.uri == "/pages/blockchain_payment_id.html") {
		response.r.headers.push_back({"Content-Type", "text/html; charset=UTF-8"});
		response.r.status = 200;
		response.set_body(std::string(payid_html));
		return true;
	}
	if (request.r.uri == "/block") {
		response.r.headers.push_back({"Content-Type", "text/html; charset=UTF-8"});
		response.r.status = 200;
		response.set_body(std::string(block_hash_html));
		return true;
	}
	if (request.r.uri == "/tx") {
		response.r.headers.push_back({"Content-Type", "text/html; charset=UTF-8"});
		response.r.status = 200;
		response.set_body(std::string(tx_hash_html));
		return true;
	}
	if (request.r.uri == "/payid") {
		response.r.headers.push_back({"Content-Type", "text/html; charset=UTF-8"});
		response.r.status = 200;
		response.set_body(std::string(payid_hash_html));
		return true;
	}
	
	auto it = m_http_handlers.find(request.r.uri);
	if (it == m_http_handlers.end()) {
		auto leg = api::bytecoind::legacy_bin_methods();
		if (std::find(leg.begin(), leg.end(), request.r.uri) != leg.end())
			response.r.status = 410;
		else
			response.r.status = 404;
		return true;
	}
	if (!m_config.bytecoind_authorization.empty() &&
	    request.r.basic_authorization != m_config.bytecoind_authorization) {
		response.r.headers.push_back({"WWW-Authenticate", "Basic realm=\"Blockchain\", charset=\"UTF-8\""});
		response.r.status = 401;
		return true;
	}
	if (!it->second(this, who, std::move(request), response))
		return false;
	response.r.status = 200;
	return true;
}

void Node::on_api_http_disconnect(http::Client *who) {
	for (auto lit = m_long_poll_http_clients.begin(); lit != m_long_poll_http_clients.end();)
		if (lit->original_who == who)
			lit = m_long_poll_http_clients.erase(lit);
		else
			++lit;
}

namespace {

template<typename CommandRequest, typename CommandResponse>
Node::HTTPHandlerFunction bin_method(bool (Node::*handler)(http::Client *who, http::RequestData &&raw_request,
    json_rpc::Request &&raw_js_request, CommandRequest &&, CommandResponse &)) {
	return [handler](Node *obj, http::Client *who, http::RequestData &&request, http::ResponseData &response) {

		CommandRequest req{};
		CommandResponse res{};

		seria::from_binary(req, request.body);

		bool result = (obj->*handler)(who, std::move(request), json_rpc::Request(), std::move(req), res);
		if (result) {
			response.set_body(seria::to_binary_str(res));
			response.r.status = 200;
		}
		return result;
	};
}
}  // anonymous namespace

const std::unordered_map<std::string, Node::HTTPHandlerFunction> Node::m_http_handlers = {

    {api::bytecoind::SyncBlocks::bin_method(), bin_method(&Node::on_wallet_sync3)},
    {api::bytecoind::SyncMemPool::bin_method(), bin_method(&Node::on_sync_mempool3)},
    {"/json_rpc", std::bind(&Node::process_json_rpc_request, std::placeholders::_1, std::placeholders::_2,
                      std::placeholders::_3, std::placeholders::_4)}};

std::unordered_map<std::string, Node::JSONRPCHandlerFunction> Node::m_jsonrpc_handlers = {
    {api::bytecoind::GetLastBlockHeaderLegacy::method(), json_rpc::make_member_method(&Node::on_get_last_block_header)},
    {api::bytecoind::GetBlockHeaderByHashLegacy::method(),
        json_rpc::make_member_method(&Node::on_get_block_header_by_hash)},
    {api::bytecoind::GetBlockHeaderByHeightLegacy::method(),
        json_rpc::make_member_method(&Node::on_get_block_header_by_height)},
    {api::bytecoind::GetBlockTemplate::method(), json_rpc::make_member_method(&Node::on_getblocktemplate)},
    {api::bytecoind::GetBlockTemplate::method_legacy(), json_rpc::make_member_method(&Node::on_getblocktemplate)},
    {api::bytecoind::GetCurrencyId::method(), json_rpc::make_member_method(&Node::on_get_currency_id)},
    {api::bytecoind::GetCurrencyId::method_legacy(), json_rpc::make_member_method(&Node::on_get_currency_id)},
    {api::bytecoind::SubmitBlock::method(), json_rpc::make_member_method(&Node::on_submitblock)},
    {api::bytecoind::SubmitBlockLegacy::method(), json_rpc::make_member_method(&Node::on_submitblock_legacy)},
    {api::bytecoind::GetRandomOutputs::method(), json_rpc::make_member_method(&Node::on_get_random_outputs3)},
    {api::bytecoind::GetStatus::method(), json_rpc::make_member_method(&Node::on_get_status3)},
    {api::bytecoind::GetStatus::method2(), json_rpc::make_member_method(&Node::on_get_status3)},
    {api::bytecoind::GetStatistics::method(), json_rpc::make_member_method(&Node::on_get_statistics)},
    {api::bytecoind::GetArchive::method(), json_rpc::make_member_method(&Node::on_get_archive)},
    {api::bytecoind::SendTransaction::method(), json_rpc::make_member_method(&Node::handle_send_transaction3)},
    {api::bytecoind::CheckSendProof::method(), json_rpc::make_member_method(&Node::handle_check_sendproof3)},
    {api::bytecoind::GetRawBlock::method(), json_rpc::make_member_method(&Node::on_get_raw_block)},
    {api::bytecoind::SyncBlocks::method(), json_rpc::make_member_method(&Node::on_wallet_sync3)},
    {api::bytecoind::GetRawTransaction::method(), json_rpc::make_member_method(&Node::on_get_raw_transaction3)},
    {api::bytecoind::SyncMemPool::method(), json_rpc::make_member_method(&Node::on_sync_mempool3)}};

bool Node::on_get_random_outputs3(http::Client *, http::RequestData &&, json_rpc::Request &&,
    api::bytecoind::GetRandomOutputs::Request &&request, api::bytecoind::GetRandomOutputs::Response &response) {
	if (request.confirmed_height_or_depth < 0)
		request.confirmed_height_or_depth = std::max(
		    0, static_cast<api::HeightOrDepth>(m_block_chain.get_tip_height()) + 1 + request.confirmed_height_or_depth);
	api::BlockHeader tip_header = m_block_chain.get_tip();
	for (uint64_t amount : request.amounts) {
		auto random_outputs = m_block_chain.get_random_outputs(
		    amount, request.outs_count, request.confirmed_height_or_depth, tip_header.timestamp);
		auto &outs = response.outputs[amount];
		outs.insert(outs.end(), random_outputs.begin(), random_outputs.end());
	}
	return true;
}

api::bytecoind::GetStatus::Response Node::create_status_response3() const {
	api::bytecoind::GetStatus::Response res;
	res.top_block_height       = m_block_chain.get_tip_height();
	res.top_known_block_height = m_downloader.get_known_block_count(res.top_block_height);
	res.top_known_block_height =
	    std::max<Height>(res.top_known_block_height, m_block_chain.internal_import_known_height());
	if (m_block_chain_reader1)
		res.top_known_block_height =
		    std::max<Height>(res.top_known_block_height, m_block_chain_reader1->get_block_count());
	if (m_block_chain_reader2)
		res.top_known_block_height =
		    std::max<Height>(res.top_known_block_height, m_block_chain_reader2->get_block_count());
	res.incoming_peer_count              = static_cast<uint32_t>(m_p2p.good_clients(true).size());
	res.outgoing_peer_count              = static_cast<uint32_t>(m_p2p.good_clients(false).size());
	api::BlockHeader tip                 = m_block_chain.get_tip();
	res.top_block_hash                   = m_block_chain.get_tip_bid();
	res.top_block_timestamp              = tip.timestamp;
	res.top_block_difficulty             = tip.difficulty;
    res.top_block_cumulative_difficulty  = tip.cumulative_difficulty;
    res.recommended_fee_per_byte         = m_block_chain.get_currency().coin() / 1000000;  // TODO - calculate
	res.next_block_effective_median_size = m_block_chain.get_next_effective_median_size();
	res.transaction_pool_version         = m_block_chain.get_tx_pool_version();
	return res;
}

bool Node::on_get_status3(http::Client *who, http::RequestData &&raw_request, json_rpc::Request &&raw_js_request,
    api::bytecoind::GetStatus::Request &&req, api::bytecoind::GetStatus::Response &res) {
	res = create_status_response3();
	if (req == res) {
		//		m_log(logging::INFO) << "on_get_status3 will long poll, json="
		//<<
		// raw_request.body << std::endl;
		LongPollClient lpc;
		lpc.original_who          = who;
		lpc.original_request      = raw_request;
		lpc.original_json_request = std::move(raw_js_request);
		lpc.original_get_status   = req;
		m_long_poll_http_clients.push_back(lpc);
		return false;
	}
	return true;
}

bool Node::on_get_statistics(http::Client *, http::RequestData &&, json_rpc::Request &&,
    api::bytecoind::GetStatistics::Request &&, api::bytecoind::GetStatistics::Response &res) {
	res.peer_id     = m_p2p.get_unique_number();
	res.platform    = platform::get_platform_name();
	res.version     = bytecoin::app_version();
	res.start_time  = m_start_time;
	res.checkpoints = m_block_chain.get_latest_checkpoints();
	return true;
}

bool Node::on_get_archive(http::Client *, http::RequestData &&, json_rpc::Request &&,
    api::bytecoind::GetArchive::Request &&req, api::bytecoind::GetArchive::Response &resp) {
	m_block_chain.read_archive(std::move(req), resp);
	return true;
}
bool Node::on_wallet_sync3(http::Client *, http::RequestData &&, json_rpc::Request &&json_req,
    api::bytecoind::SyncBlocks::Request &&req, api::bytecoind::SyncBlocks::Response &res) {
	if (req.sparse_chain.empty())
		throw std::runtime_error("Empty sparse chain - must include at least genesis block");
	if (req.sparse_chain.back() != m_block_chain.get_genesis_bid())
		throw std::runtime_error(
		    "Wrong currency - different genesis block. Must be " + common::pod_to_hex(m_block_chain.get_genesis_bid()));
	if (req.max_count > api::bytecoind::SyncBlocks::Request::MAX_COUNT)
		throw std::runtime_error(
		    "Too big max_count - must be < " + common::to_string(api::bytecoind::SyncBlocks::Request::MAX_COUNT));
	auto first_block_timestamp = req.first_block_timestamp < m_block_chain.get_currency().block_future_time_limit
	                                 ? 0
	                                 : req.first_block_timestamp - m_block_chain.get_currency().block_future_time_limit;
	Height full_offset = m_block_chain.get_timestamp_lower_bound_block_index(first_block_timestamp);
	Height start_block_index;
	std::vector<crypto::Hash> supplement =
	    m_block_chain.get_sync_headers_chain(req.sparse_chain, &start_block_index, req.max_count);
	if (full_offset >= start_block_index + supplement.size()) {
		start_block_index = full_offset;
		supplement.clear();
		while (supplement.size() < req.max_count) {
			Hash ha;
			if (!m_block_chain.read_chain(start_block_index + static_cast<Height>(supplement.size()), &ha))
				break;
			supplement.push_back(ha);
		}
	} else if (full_offset > start_block_index) {
		supplement.erase(supplement.begin(), supplement.begin() + (full_offset - start_block_index));
		start_block_index = full_offset;
	}

	res.start_height = start_block_index;
	res.blocks.resize(supplement.size());
	for (size_t i = 0; i != supplement.size(); ++i) {
		auto bhash = supplement[i];
		invariant(
		    m_block_chain.read_header(bhash, &res.blocks[i].header), "Block header must be there, but it is not there");
		BlockChainState::BlockGlobalIndices global_indices;
		// if (res.blocks[i].header.timestamp >= req.first_block_timestamp) //
		// commented out becuase empty Block cannot be serialized
		{
			RawBlock rb;
			invariant(m_block_chain.read_block(bhash, &rb), "Block must be there, but it is not there");
			Block block;
			invariant(block.from_raw_block(rb), "RawBlock failed to convert into block");
			res.blocks[i].base_transaction_hash = get_transaction_hash(block.header.base_transaction);
			res.blocks[i].raw_header            = std::move(block.header);
			res.blocks[i].raw_transactions.reserve(block.transactions.size());
			res.blocks[i].transaction_binary_sizes.reserve(block.transactions.size());
			for (size_t tx_index = 0; tx_index != block.transactions.size(); ++tx_index) {
				res.blocks[i].raw_transactions.push_back(std::move(block.transactions.at(tx_index)));
				res.blocks[i].transaction_binary_sizes.push_back(
				    static_cast<uint32_t>(rb.transactions.at(tx_index).size()));
			}
			invariant(m_block_chain.read_block_output_global_indices(bhash, &res.blocks[i].global_indices),
			    "Invariant dead - bid is in chain but blockchain has no block indices");
		}
	}
	res.status = create_status_response3();
	return true;
}

bool Node::on_sync_mempool3(http::Client *, http::RequestData &&, json_rpc::Request &&,
    api::bytecoind::SyncMemPool::Request &&req, api::bytecoind::SyncMemPool::Response &res) {
	const auto &pool = m_block_chain.get_memory_state_transactions();
	for (auto &&ex : req.known_hashes)
		if (pool.count(ex) == 0)
			res.removed_hashes.push_back(ex);
	for (auto &&tx : pool)
		if (!std::binary_search(req.known_hashes.begin(), req.known_hashes.end(), tx.first)) {
			//			res.added_binary_transactions.push_back(seria::to_binary(tx.second));
			res.added_raw_transactions.push_back(tx.second.tx);
			res.added_transactions.push_back(api::Transaction{});
			res.added_transactions.back().hash        = tx.first;
			res.added_transactions.back().timestamp   = tx.second.timestamp;
			res.added_transactions.back().fee         = tx.second.fee;
			res.added_transactions.back().binary_size = static_cast<uint32_t>(tx.second.binary_tx.size());
		}
	res.status = create_status_response3();
	return true;
}

bool Node::on_get_raw_block(http::Client *, http::RequestData &&, json_rpc::Request &&,
                      api::bytecoind::GetRawBlock::Request && request, api::bytecoind::GetRawBlock::Response & response){

    if (!m_block_chain.read_header(request.hash, &response.header))
        throw json_rpc::Error(-5, "Block not found by hash"); // TODO - use HASH_NOT_FOUND enum
    BlockChainState::BlockGlobalIndices global_indices;

    RawBlock rb;
    invariant(m_block_chain.read_block(request.hash, &rb), "Block must be there, but it is not there");
    Block block;
    invariant(block.from_raw_block(rb), "RawBlock failed to convert into block");

    auto coinbase_size = static_cast<uint32_t>(seria::binary_size(block.header.base_transaction));
    response.header.transactions_cumulative_size = response.header.block_size;

    response.header.block_size = response.header.transactions_cumulative_size + static_cast<uint32_t>(rb.block.size()) - coinbase_size;
    response.base_transaction_hash = get_transaction_hash(block.header.base_transaction);
    response.raw_header            = std::move(block.header);
    response.raw_transactions.reserve(block.transactions.size());
    response.transaction_binary_sizes.reserve(block.transactions.size() + 1);
    response.transaction_binary_sizes.push_back(coinbase_size);
    for (size_t tx_index = 0; tx_index != block.transactions.size(); ++tx_index) {
        response.raw_transactions.push_back(std::move(block.transactions.at(tx_index)));
        response.transaction_binary_sizes.push_back(
                static_cast<uint32_t>(rb.transactions.at(tx_index).size()));
    }
    m_block_chain.read_block_output_global_indices(request.hash, &response.global_indices);
    // If block not in main chain - global indices will be empty
    return true;
 }

bool Node::on_get_raw_transaction3(http::Client *, http::RequestData &&, json_rpc::Request &&,
    api::bytecoind::GetRawTransaction::Request &&req, api::bytecoind::GetRawTransaction::Response &res) {
	const auto &pool = m_block_chain.get_memory_state_transactions();
	auto tit         = pool.find(req.hash);
	if (tit != pool.end()) {
		res.raw_transaction          = static_cast<TransactionPrefix>(tit->second.tx);
		res.transaction.fee          = tit->second.fee;
		res.transaction.hash         = req.hash;
		res.transaction.block_height = m_block_chain.get_tip_height() + 1;
		res.transaction.timestamp    = tit->second.timestamp;
		res.transaction.binary_size  = static_cast<uint32_t>(tit->second.binary_tx.size());
		return true;
	}
	Transaction tx;
	size_t index_in_block = 0;
	if (m_block_chain.read_transaction(req.hash, &tx, &res.transaction.block_height, &res.transaction.block_hash,
	        &index_in_block, &res.transaction.binary_size)) {
		res.raw_transaction  = static_cast<TransactionPrefix>(tx);  // TODO - std::move?
		res.transaction.hash = req.hash;
		res.transaction.fee  = get_tx_fee(res.raw_transaction);  // 0 for coinbase
		return true;
	}
	return true;
}

bool Node::handle_send_transaction3(http::Client *, http::RequestData &&, json_rpc::Request &&,
    api::bytecoind::SendTransaction::Request &&request, api::bytecoind::SendTransaction::Response &response) {
	response.send_result = "broadcast";

	NOTIFY_NEW_TRANSACTIONS::request msg;
	Height conflict_height =
	    m_block_chain.get_currency().max_block_height;  // So will not be accidentally viewed as confirmed
	Transaction tx;
	try {
		seria::from_binary(tx, request.binary_transaction);
	} catch (const std::exception &ex) {
		api::bytecoind::SendTransaction::Error err;
		err.code            = api::bytecoind::SendTransaction::INVALID_TRANSACTION_BINARY_FORMAT;
		err.message         = ex.what();
		err.conflict_height = conflict_height;
		throw err;
	}
	const Hash tid = get_transaction_hash(tx);
	auto action    = m_block_chain.add_transaction(
        tid, tx, request.binary_transaction, m_p2p.get_local_time(), &conflict_height, "json_rpc");
	switch (action) {
	case AddTransactionResult::BAN:
		throw json_rpc::Error(
		    api::bytecoind::SendTransaction::INVALID_TRANSACTION_BINARY_FORMAT, "Binary transaction format is wrong");
	case AddTransactionResult::BROADCAST_ALL: {
		msg.txs.push_back(request.binary_transaction);
		BinaryArray raw_msg =
		    LevinProtocol::send_message(NOTIFY_NEW_TRANSACTIONS::ID, LevinProtocol::encode(msg), false);
		m_p2p.broadcast(nullptr, raw_msg);
		advance_long_poll();
		break;
	}
	case AddTransactionResult::ALREADY_IN_POOL:
		break;
	case AddTransactionResult::INCREASE_FEE:
		break;
	case AddTransactionResult::FAILED_TO_REDO: {
		api::bytecoind::SendTransaction::Error err;
		err.code            = api::bytecoind::SendTransaction::WRONG_OUTPUT_REFERENCE;
		err.message         = "Transaction references outputs changed during reorganization or signature wrong";
		err.conflict_height = conflict_height;
		throw err;
	}
	case AddTransactionResult::OUTPUT_ALREADY_SPENT: {
		api::bytecoind::SendTransaction::Error err;
		err.code            = api::bytecoind::SendTransaction::OUTPUT_ALREADY_SPENT;
		err.message         = "One of referenced outputs is already spent";
		err.conflict_height = conflict_height;
		throw err;
	}
    case AddTransactionResult::TOO_OLD:
        api::bytecoind::SendTransaction::Error err;
        err.code            = api::bytecoind::SendTransaction::TOO_OLD;
        err.message         = "Trying to send too old transaction";
        err.conflict_height = conflict_height;
        throw err;
	}
	return true;
}

bool Node::handle_check_sendproof3(http::Client *, http::RequestData &&, json_rpc::Request &&,
    api::bytecoind::CheckSendProof::Request &&request, api::bytecoind::CheckSendProof::Response &response) {
	Transaction tx;
	SendProof sp;
	try {
		seria::from_json_value(sp, common::JsonValue::from_string(request.sendproof));
	} catch (const std::exception &ex) {
		throw api::bytecoind::CheckSendProof::Error(api::bytecoind::CheckSendProof::FAILED_TO_PARSE,
		    "Failed to parse proof object ex.what=" + std::string(ex.what()));
	}
	Height height = 0;
	Hash block_hash;
	size_t index_in_block = 0;
	uint32_t binary_size  = 0;
	if (!m_block_chain.read_transaction(
	        sp.transaction_hash, &tx, &height, &block_hash, &index_in_block, &binary_size)) {
		throw api::bytecoind::CheckSendProof::Error(
		    api::bytecoind::CheckSendProof::NOT_IN_MAIN_CHAIN, "Transaction is not in main chain");
	}
	PublicKey tx_public_key = get_transaction_public_key_from_extra(tx.extra);
	Hash message_hash       = crypto::cn_fast_hash(sp.message.data(), sp.message.size());
	if (!crypto::check_sendproof(
	        tx_public_key, sp.address.view_public_key, sp.derivation, message_hash, sp.signature)) {
		throw api::bytecoind::CheckSendProof::Error(api::bytecoind::CheckSendProof::WRONG_SIGNATURE,
		    "Proof object does not match transaction or was tampered with");
	}
	Amount total_amount = 0;
	size_t key_index    = 0;
	uint32_t out_index  = 0;
	for (const auto &output : tx.outputs) {
		if (output.target.type() == typeid(KeyOutput)) {
			const KeyOutput &key_output = boost::get<KeyOutput>(output.target);
			PublicKey spend_key;
			if (underive_public_key(sp.derivation, key_index, key_output.key, spend_key) &&
			    spend_key == sp.address.spend_public_key) {
				total_amount += output.amount;
			}
			++key_index;
		}
		++out_index;
	}
	if (total_amount == 0)
		throw api::bytecoind::CheckSendProof::Error(api::bytecoind::CheckSendProof::ADDRESS_NOT_IN_TRANSACTION,
		    "No outputs found in transaction for the address being proofed");
	if (total_amount != sp.amount)
		throw api::bytecoind::CheckSendProof::Error(api::bytecoind::CheckSendProof::WRONG_AMOUNT,
		    "Wrong amount in outputs, actual amount is " + common::to_string(total_amount));
	return true;
}
