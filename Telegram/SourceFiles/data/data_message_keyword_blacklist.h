/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_map.h"
#include "base/flat_set.h"

class HistoryItem;
class PeerData;
struct TextWithEntities;

namespace Data {

class Session;

class MessageKeywordBlacklist final {
public:
	using ChannelKeywords = base::flat_map<PeerId, std::vector<QString>>;

	explicit MessageKeywordBlacklist(not_null<Session*> owner);
	~MessageKeywordBlacklist();

	[[nodiscard]] const std::vector<QString> &commonKeywords() const;
	[[nodiscard]] const std::vector<QString> &commonLinks() const;
	[[nodiscard]] const std::vector<QString> &commonTextLinks() const;
	[[nodiscard]] const std::vector<QString> &channelKeywords(
		PeerId peerId) const;
	void setCommonKeywords(std::vector<QString> keywords);
	void setCommonLinks(std::vector<QString> links);
	void setCommonTextLinks(std::vector<QString> links);
	void setChannelKeywords(PeerId peerId, std::vector<QString> keywords);

	[[nodiscard]] bool hasRules(PeerId peerId) const;
	[[nodiscard]] bool matches(not_null<HistoryItem*> item) const;
	[[nodiscard]] std::vector<std::pair<int, int>> blockedLinkRanges(
		const TextWithEntities &text) const;
	[[nodiscard]] bool isCollapsed(not_null<HistoryItem*> item) const;
	[[nodiscard]] bool isExpanded(FullMsgId itemId) const;
	void setExpanded(not_null<HistoryItem*> item, bool expanded);
	void toggleExpanded(not_null<HistoryItem*> item);

	void track(not_null<HistoryItem*> item);
	void untrack(not_null<HistoryItem*> item);
	void updateItemId(FullMsgId wasId, FullMsgId nowId);
	void recomputeLoaded();

	[[nodiscard]] std::vector<not_null<HistoryItem*>> matchedItems(
		PeerData *peer = nullptr) const;
	[[nodiscard]] rpl::producer<> changed() const;

private:
	static std::vector<QString> NormalizeKeywords(
		std::vector<QString> keywords);
	[[nodiscard]] bool updateIndexed(
		not_null<HistoryItem*> item,
		const base::flat_map<PeerId, base::flat_set<MsgId>> &was);
	void save();

	const not_null<Session*> _owner;
	std::vector<QString> _commonKeywords;
	std::vector<QString> _commonKeywordsFolded;
	std::vector<QString> _commonLinks;
	std::vector<QString> _commonLinksNormalized;
	std::vector<QString> _commonTextLinks;
	std::vector<QString> _commonTextLinksFolded;
	ChannelKeywords _channelKeywords;
	ChannelKeywords _channelKeywordsFolded;
	base::flat_map<PeerId, base::flat_set<MsgId>> _matchedByPeer;
	base::flat_set<FullMsgId> _expanded;
	rpl::event_stream<> _changed;

};

} // namespace Data
