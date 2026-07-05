/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_message_keyword_blacklist.h"

#include "data/data_session.h"
#include "data/data_channel.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"

namespace Data {
namespace {

const auto kEmptyKeywords = std::vector<QString>();

[[nodiscard]] bool IsLinkEntity(EntityType type) {
	return (type == EntityType::Url)
		|| (type == EntityType::Email);
}

[[nodiscard]] QString TextForMatching(const TextWithEntities &text) {
	auto result = text.text;
	auto ranges = std::vector<std::pair<int, int>>();
	for (const auto &entity : text.entities) {
		if (IsLinkEntity(entity.type()) && entity.length() > 0) {
			ranges.emplace_back(entity.offset(), entity.length());
		}
	}
	ranges::sort(ranges, ranges::greater(), &std::pair<int, int>::first);
	for (const auto &[offset, length] : ranges) {
		if (offset >= 0 && offset < result.size()) {
			result.remove(offset, std::min(length, result.size() - offset));
		}
	}
	return result.toCaseFolded();
}

[[nodiscard]] bool ContainsKeyword(
		const QString &text,
		const std::vector<QString> &keywords) {
	for (const auto &keyword : keywords) {
		if (text.contains(keyword)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool Contains(
		const base::flat_map<PeerId, base::flat_set<MsgId>> &map,
		FullMsgId itemId) {
	const auto i = map.find(itemId.peer);
	return (i != end(map)) && i->second.contains(itemId.msg);
}

[[nodiscard]] bool IsBroadcastMessage(not_null<HistoryItem*> item) {
	return item->isRegular()
		&& item->history()
		&& item->history()->peer
		&& item->history()->peer->asBroadcast();
}

[[nodiscard]] std::vector<QString> FoldKeywords(
		const std::vector<QString> &keywords) {
	auto result = std::vector<QString>();
	result.reserve(keywords.size());
	for (const auto &keyword : keywords) {
		result.push_back(keyword.toCaseFolded());
	}
	return result;
}

} // namespace

MessageKeywordBlacklist::MessageKeywordBlacklist(not_null<Session*> owner)
: _owner(owner)
, _commonKeywords(NormalizeKeywords(
	owner->session().settings().messageBlacklistCommonKeywords()))
, _channelKeywords(
	owner->session().settings().messageBlacklistChannelKeywords()) {
	_commonKeywordsFolded = FoldKeywords(_commonKeywords);
	for (auto i = begin(_channelKeywords); i != end(_channelKeywords);) {
		i->second = NormalizeKeywords(std::move(i->second));
		if (i->second.empty()) {
			i = _channelKeywords.erase(i);
		} else {
			_channelKeywordsFolded.emplace(i->first, FoldKeywords(i->second));
			++i;
		}
	}
}

MessageKeywordBlacklist::~MessageKeywordBlacklist() = default;

const std::vector<QString> &MessageKeywordBlacklist::commonKeywords() const {
	return _commonKeywords;
}

const std::vector<QString> &MessageKeywordBlacklist::channelKeywords(
		PeerId peerId) const {
	const auto i = _channelKeywords.find(peerId);
	return (i != end(_channelKeywords)) ? i->second : kEmptyKeywords;
}

void MessageKeywordBlacklist::setCommonKeywords(
		std::vector<QString> keywords) {
	keywords = NormalizeKeywords(std::move(keywords));
	if (_commonKeywords == keywords) {
		return;
	}
	_commonKeywords = std::move(keywords);
	_commonKeywordsFolded = FoldKeywords(_commonKeywords);
	save();
	recomputeLoaded();
}

void MessageKeywordBlacklist::setChannelKeywords(
		PeerId peerId,
		std::vector<QString> keywords) {
	keywords = NormalizeKeywords(std::move(keywords));
	const auto i = _channelKeywords.find(peerId);
	if (keywords.empty()) {
		if (i == end(_channelKeywords)) {
			return;
		}
		_channelKeywords.erase(i);
		_channelKeywordsFolded.remove(peerId);
	} else if (i != end(_channelKeywords)) {
		if (i->second == keywords) {
			return;
		}
		i->second = std::move(keywords);
		_channelKeywordsFolded[peerId] = FoldKeywords(i->second);
	} else {
		const auto folded = FoldKeywords(keywords);
		_channelKeywords.emplace(peerId, std::move(keywords));
		_channelKeywordsFolded.emplace(peerId, std::move(folded));
	}
	save();
	recomputeLoaded();
}

bool MessageKeywordBlacklist::hasRules(PeerId peerId) const {
	if (!_commonKeywords.empty()) {
		return true;
	}
	const auto i = _channelKeywords.find(peerId);
	return (i != end(_channelKeywords)) && !i->second.empty();
}

bool MessageKeywordBlacklist::matches(not_null<HistoryItem*> item) const {
	if (!IsBroadcastMessage(item)) {
		return false;
	}
	const auto peerId = item->history()->peer->id;
	const auto i = _channelKeywordsFolded.find(peerId);
	if (_commonKeywordsFolded.empty()
		&& (i == end(_channelKeywordsFolded) || i->second.empty())) {
		return false;
	}
	const auto text = TextForMatching(item->originalText());
	return !text.isEmpty()
		&& (ContainsKeyword(text, _commonKeywordsFolded)
			|| (i != end(_channelKeywordsFolded)
				&& ContainsKeyword(text, i->second)));
}

bool MessageKeywordBlacklist::isCollapsed(not_null<HistoryItem*> item) const {
	return matches(item) && !isExpanded(item->fullId());
}

bool MessageKeywordBlacklist::isExpanded(FullMsgId itemId) const {
	return _expanded.contains(itemId);
}

void MessageKeywordBlacklist::setExpanded(
		not_null<HistoryItem*> item,
		bool expanded) {
	const auto itemId = item->fullId();
	const auto changed = expanded
		? _expanded.emplace(itemId).second
		: _expanded.remove(itemId);
	if (changed) {
		_owner->requestItemResize(item);
	}
}

void MessageKeywordBlacklist::toggleExpanded(not_null<HistoryItem*> item) {
	setExpanded(item, !isExpanded(item->fullId()));
}

void MessageKeywordBlacklist::track(not_null<HistoryItem*> item) {
	const auto was = _matchedByPeer;
	if (updateIndexed(item, was)) {
		_changed.fire({});
	}
}

void MessageKeywordBlacklist::untrack(not_null<HistoryItem*> item) {
	const auto itemId = item->fullId();
	const auto i = _matchedByPeer.find(itemId.peer);
	auto changed = false;
	if (i != end(_matchedByPeer)) {
		changed = i->second.remove(itemId.msg);
		if (i->second.empty()) {
			_matchedByPeer.erase(i);
		}
	}
	_expanded.remove(itemId);
	if (changed) {
		_changed.fire({});
	}
}

void MessageKeywordBlacklist::updateItemId(
		FullMsgId wasId,
		FullMsgId nowId) {
	const auto i = _matchedByPeer.find(wasId.peer);
	if (i != end(_matchedByPeer)) {
		if (i->second.remove(wasId.msg)) {
			if (i->second.empty()) {
				_matchedByPeer.erase(i);
			}
			_matchedByPeer[nowId.peer].emplace(nowId.msg);
		}
	}
	if (_expanded.remove(wasId)) {
		_expanded.emplace(nowId);
	}
}

void MessageKeywordBlacklist::recomputeLoaded() {
	const auto was = base::take(_matchedByPeer);
	_owner->enumerateLoadedMessages([&](not_null<HistoryItem*> item) {
		const auto changed = updateIndexed(item, was);
		(void)changed;
	});
	_changed.fire({});
}

std::vector<not_null<HistoryItem*>> MessageKeywordBlacklist::matchedItems(
		PeerData *peer) const {
	auto result = std::vector<not_null<HistoryItem*>>();
	for (const auto &[peerId, messages] : _matchedByPeer) {
		if (peer && peer->id != peerId) {
			continue;
		}
		for (const auto &msgId : messages) {
			if (const auto item = _owner->message(peerId, msgId)) {
				result.push_back(item);
			}
		}
	}
	ranges::sort(result, [](not_null<HistoryItem*> a, not_null<HistoryItem*> b) {
		return std::make_pair(a->date(), a->id)
			> std::make_pair(b->date(), b->id);
	});
	return result;
}

rpl::producer<> MessageKeywordBlacklist::changed() const {
	return _changed.events();
}

std::vector<QString> MessageKeywordBlacklist::NormalizeKeywords(
		std::vector<QString> keywords) {
	auto result = std::vector<QString>();
	auto seen = base::flat_set<QString>();
	result.reserve(keywords.size());
	for (auto &keyword : keywords) {
		keyword = keyword.trimmed();
		const auto folded = keyword.toCaseFolded();
		if (!keyword.isEmpty() && seen.emplace(folded).second) {
			result.push_back(std::move(keyword));
		}
	}
	return result;
}

bool MessageKeywordBlacklist::updateIndexed(
		not_null<HistoryItem*> item,
		const base::flat_map<PeerId, base::flat_set<MsgId>> &was) {
	const auto itemId = item->fullId();
	const auto now = matches(item);
	if (now) {
		_matchedByPeer[itemId.peer].emplace(itemId.msg);
	} else {
		if (const auto i = _matchedByPeer.find(itemId.peer);
				i != end(_matchedByPeer)) {
			const auto removed = i->second.remove(itemId.msg);
			(void)removed;
			if (i->second.empty()) {
				_matchedByPeer.erase(i);
			}
		}
		_expanded.remove(itemId);
	}
	const auto before = Contains(was, itemId);
	if (before != now) {
		_owner->requestItemResize(item);
		return true;
	}
	return false;
}

void MessageKeywordBlacklist::save() {
	_owner->session().settings().setMessageBlacklistCommonKeywords(
		_commonKeywords);
	_owner->session().settings().setMessageBlacklistChannelKeywords(
		_channelKeywords);
	_owner->session().saveSettingsDelayed();
}

} // namespace Data
