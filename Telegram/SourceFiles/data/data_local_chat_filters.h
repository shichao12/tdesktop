/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_map.h"

class History;

namespace Dialogs {
class Key;
class MainList;
} // namespace Dialogs

namespace Data {

class Session;

struct LocalChatFilter final {
	int id = 0;
	QString title;
	std::vector<PeerId> peers;
	std::vector<PeerId> pinnedPeers;
};

[[nodiscard]] bool IsLocalChatFilterRuntimeId(FilterId id);
[[nodiscard]] bool IsLocalChatFilterListId(FilterId id);
[[nodiscard]] FilterId LocalChatFilterAllRuntimeId();
[[nodiscard]] FilterId LocalChatFilterListId(FilterId id);
[[nodiscard]] FilterId LocalChatFilterRuntimeId(int id);
[[nodiscard]] int LocalChatFilterIdFromRuntime(FilterId id);

class LocalChatFilters final {
public:
	explicit LocalChatFilters(not_null<Session*> owner);
	~LocalChatFilters();

	[[nodiscard]] const std::vector<LocalChatFilter> &list() const;
	[[nodiscard]] bool has() const;
	[[nodiscard]] rpl::producer<> changed() const;

	[[nodiscard]] const LocalChatFilter *lookup(int id) const;
	[[nodiscard]] const LocalChatFilter *lookupRuntime(FilterId id) const;
	[[nodiscard]] not_null<Dialogs::MainList*> chatsList(FilterId id);
	[[nodiscard]] bool containsAny(not_null<History*> history) const;
	[[nodiscard]] int pinnedLimit(FilterId id) const;
	[[nodiscard]] bool pinnedCanPin(
		FilterId id,
		not_null<History*> history);
	[[nodiscard]] const std::vector<Dialogs::Key> &pinnedOrder(FilterId id);
	void setChatPinned(Dialogs::Key key, FilterId id, bool pinned);
	void reorderPinned(
		FilterId id,
		Dialogs::Key key1,
		Dialogs::Key key2);

	int create(QString title, History *history = nullptr);
	void rename(int id, QString title);
	void remove(int id);
	void reorder(int oldPosition, int newPosition);
	void savePinnedOrder(FilterId id);

	[[nodiscard]] bool contains(int id, not_null<History*> history) const;
	[[nodiscard]] bool containsRuntime(
		FilterId id,
		not_null<History*> history) const;
	void setContains(int id, not_null<History*> history, bool add);
	void refreshHistory(not_null<History*> history);
	void refreshPinnedOrder(not_null<History*> history);

private:
	void save(bool notify = true);
	[[nodiscard]] LocalChatFilter *lookupMutable(int id);
	[[nodiscard]] std::vector<PeerId> *pinnedPeers(FilterId id);
	void applyPinnedOrder(FilterId id);

	const not_null<Session*> _owner;
	std::vector<LocalChatFilter> _list;
	std::vector<PeerId> _allPinnedPeers;
	base::flat_map<FilterId, std::unique_ptr<Dialogs::MainList>> _chatsLists;
	rpl::event_stream<> _changed;
	int _pinnedOrderChangeDepth = 0;
	int _nextId = 1;

};

} // namespace Data
