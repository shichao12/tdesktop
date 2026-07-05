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

[[nodiscard]] bool IsBlockedLinkEntity(EntityType type) {
	return (type == EntityType::Url)
		|| (type == EntityType::CustomUrl);
}

[[nodiscard]] QString NormalizeLink(QString link) {
	link = link.trimmed().toCaseFolded();
	while (link.endsWith('/')) {
		link.chop(1);
	}
	return link;
}

[[nodiscard]] std::vector<QString> NormalizeLinks(
		std::vector<QString> links) {
	auto result = std::vector<QString>();
	auto seen = base::flat_set<QString>();
	result.reserve(links.size());
	for (auto &link : links) {
		link = link.trimmed();
		const auto normalized = NormalizeLink(link);
		if (!normalized.isEmpty() && seen.emplace(normalized).second) {
			result.push_back(std::move(link));
		}
	}
	return result;
}

[[nodiscard]] std::vector<QString> NormalizeLinkMatches(
		const std::vector<QString> &links) {
	auto result = std::vector<QString>();
	result.reserve(links.size());
	for (const auto &link : links) {
		result.push_back(NormalizeLink(link));
	}
	return result;
}

[[nodiscard]] bool HasBlockedLinkEntities(const TextWithEntities &text) {
	for (const auto &entity : text.entities) {
		if (IsBlockedLinkEntity(entity.type())) {
			return true;
		}
	}
	return false;
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
, _commonLinks(NormalizeLinks(
	owner->session().settings().messageBlacklistCommonLinks()))
, _commonTextLinks(NormalizeKeywords(
	owner->session().settings().messageBlacklistCommonTextLinks()))
, _channelKeywords(
	owner->session().settings().messageBlacklistChannelKeywords()) {
	_commonKeywordsFolded = FoldKeywords(_commonKeywords);
	_commonLinksNormalized = NormalizeLinkMatches(_commonLinks);
	_commonTextLinksFolded = FoldKeywords(_commonTextLinks);
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

const std::vector<QString> &MessageKeywordBlacklist::commonLinks() const {
	return _commonLinks;
}

const std::vector<QString> &MessageKeywordBlacklist::commonTextLinks() const {
	return _commonTextLinks;
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

void MessageKeywordBlacklist::setCommonLinks(std::vector<QString> links) {
	links = NormalizeLinks(std::move(links));
	if (_commonLinks == links) {
		return;
	}
	_commonLinks = std::move(links);
	_commonLinksNormalized = NormalizeLinkMatches(_commonLinks);
	save();
	_owner->enumerateLoadedMessages([&](not_null<HistoryItem*> item) {
		if (HasBlockedLinkEntities(item->originalText())) {
			_owner->requestItemTextRefresh(item);
		}
	});
	_changed.fire({});
}

void MessageKeywordBlacklist::setCommonTextLinks(std::vector<QString> links) {
	links = NormalizeKeywords(std::move(links));
	if (_commonTextLinks == links) {
		return;
	}
	_commonTextLinks = std::move(links);
	_commonTextLinksFolded = FoldKeywords(_commonTextLinks);
	save();
	_owner->enumerateLoadedMessages([&](not_null<HistoryItem*> item) {
		if (HasBlockedLinkEntities(item->originalText())) {
			_owner->requestItemTextRefresh(item);
		}
	});
	_changed.fire({});
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
	if (!_commonKeywords.empty()
		|| !_commonLinks.empty()
		|| !_commonTextLinks.empty()) {
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

std::vector<std::pair<int, int>> MessageKeywordBlacklist::blockedLinkRanges(
		const TextWithEntities &text) const {
	if (_commonLinksNormalized.empty() && _commonTextLinksFolded.empty()) {
		return {};
	}
	auto result = std::vector<std::pair<int, int>>();
	for (const auto &entity : text.entities) {
		const auto offset = entity.offset();
		if (!IsBlockedLinkEntity(entity.type())
			|| offset < 0
			|| offset >= text.text.size()
			|| entity.length() <= 0) {
			continue;
		}
		const auto length = std::min(
			entity.length(),
			text.text.size() - offset);
		auto blocked = false;
		if (!_commonLinksNormalized.empty()) {
			const auto link = NormalizeLink(
				(entity.type() == EntityType::Url)
					? text.text.mid(offset, length)
					: entity.data());
			blocked = ranges::contains(_commonLinksNormalized, link);
		}
		if (!blocked
			&& entity.type() == EntityType::CustomUrl
			&& !_commonTextLinksFolded.empty()) {
			const auto textLink = text.text.mid(
				offset,
				length).trimmed().toCaseFolded();
			blocked = ranges::contains(_commonTextLinksFolded, textLink);
		}
		if (blocked) {
			result.emplace_back(offset, length);
		}
	}
	return result;
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
	_owner->session().settings().setMessageBlacklistCommonLinks(
		_commonLinks);
	_owner->session().settings().setMessageBlacklistCommonTextLinks(
		_commonTextLinks);
	_owner->session().settings().setMessageBlacklistChannelKeywords(
		_channelKeywords);
	_owner->session().saveSettingsDelayed();
}

} // namespace Data
