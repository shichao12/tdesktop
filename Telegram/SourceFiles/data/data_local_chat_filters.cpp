/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_local_chat_filters.h"

#include "data/data_session.h"
#include "dialogs/dialogs_key.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"

namespace Data {
namespace {

constexpr auto kRuntimeIdBase = FilterId(-1000000);
constexpr auto kLocalPinnedLimit = 1000;

[[nodiscard]] bool HasPeer(
		const std::vector<PeerId> &peers,
		PeerId peerId) {
	return ranges::contains(peers, peerId);
}

[[nodiscard]] bool HasPeer(
		const LocalChatFilter &filter,
		PeerId peerId) {
	return HasPeer(filter.peers, peerId);
}

} // namespace

bool IsLocalChatFilterRuntimeId(FilterId id) {
	return id < kRuntimeIdBase;
}

bool IsLocalChatFilterListId(FilterId id) {
	return (id == kRuntimeIdBase) || IsLocalChatFilterRuntimeId(id);
}

FilterId LocalChatFilterAllRuntimeId() {
	return kRuntimeIdBase;
}

FilterId LocalChatFilterListId(FilterId id) {
	return id ? id : LocalChatFilterAllRuntimeId();
}

FilterId LocalChatFilterRuntimeId(int id) {
	Expects(id > 0);

	return kRuntimeIdBase - FilterId(id);
}

int LocalChatFilterIdFromRuntime(FilterId id) {
	Expects(IsLocalChatFilterRuntimeId(id));

	return int(kRuntimeIdBase - id);
}

LocalChatFilters::LocalChatFilters(not_null<Session*> owner)
: _owner(owner)
, _list(owner->session().settings().localChatFilters())
, _allPinnedPeers(owner->session().settings().localChatFilterAllPinnedPeers())
, _nextId(owner->session().settings().nextLocalChatFilterId()) {
	for (const auto &filter : _list) {
		_nextId = std::max(_nextId, filter.id + 1);
	}
}

LocalChatFilters::~LocalChatFilters() = default;

const std::vector<LocalChatFilter> &LocalChatFilters::list() const {
	return _list;
}

bool LocalChatFilters::has() const {
	return !_list.empty();
}

rpl::producer<> LocalChatFilters::changed() const {
	return _changed.events();
}

const LocalChatFilter *LocalChatFilters::lookup(int id) const {
	const auto i = ranges::find(_list, id, &LocalChatFilter::id);
	return (i != end(_list)) ? &*i : nullptr;
}

LocalChatFilter *LocalChatFilters::lookupMutable(int id) {
	const auto i = ranges::find(_list, id, &LocalChatFilter::id);
	return (i != end(_list)) ? &*i : nullptr;
}

const LocalChatFilter *LocalChatFilters::lookupRuntime(FilterId id) const {
	return IsLocalChatFilterRuntimeId(id)
		? lookup(LocalChatFilterIdFromRuntime(id))
		: nullptr;
}

not_null<Dialogs::MainList*> LocalChatFilters::chatsList(FilterId id) {
	Expects(IsLocalChatFilterListId(id));

	auto &pointer = _chatsLists[id];
	if (!pointer) {
		pointer = std::make_unique<Dialogs::MainList>(
			&_owner->session(),
			id,
			rpl::single(kLocalPinnedLimit));
		applyPinnedOrder(id);
	}
	return pointer.get();
}

bool LocalChatFilters::containsAny(not_null<History*> history) const {
	const auto peerId = history->peer->id;
	return ranges::any_of(_list, [=](const LocalChatFilter &filter) {
		return HasPeer(filter, peerId);
	});
}

int LocalChatFilters::pinnedLimit(FilterId id) const {
	Expects(IsLocalChatFilterListId(id));

	return kLocalPinnedLimit;
}

bool LocalChatFilters::pinnedCanPin(
		FilterId id,
		not_null<History*> history) {
	Expects(IsLocalChatFilterListId(id));

	if (!history->inChatList(id)) {
		return false;
	}
	const auto &order = chatsList(id)->pinned()->order();
	return ranges::contains(order, history.get(), &Dialogs::Key::history)
		|| (order.size() < pinnedLimit(id));
}

const std::vector<Dialogs::Key> &LocalChatFilters::pinnedOrder(FilterId id) {
	Expects(IsLocalChatFilterListId(id));

	return chatsList(id)->pinned()->order();
}

void LocalChatFilters::setChatPinned(
		Dialogs::Key key,
		FilterId id,
		bool pinned) {
	Expects(IsLocalChatFilterListId(id));

	++_pinnedOrderChangeDepth;
	const auto guard = gsl::finally([&] {
		--_pinnedOrderChangeDepth;
	});
	chatsList(id)->pinned()->setPinned(key, pinned);
	savePinnedOrder(id);
}

void LocalChatFilters::reorderPinned(
		FilterId id,
		Dialogs::Key key1,
		Dialogs::Key key2) {
	Expects(IsLocalChatFilterListId(id));

	++_pinnedOrderChangeDepth;
	const auto guard = gsl::finally([&] {
		--_pinnedOrderChangeDepth;
	});
	chatsList(id)->pinned()->reorder(key1, key2);
	savePinnedOrder(id);
}

int LocalChatFilters::create(QString title, History *history) {
	const auto id = _nextId++;
	auto peers = std::vector<PeerId>();
	if (history) {
		peers.push_back(history->peer->id);
	}
	_list.push_back({
		.id = id,
		.title = std::move(title),
		.peers = std::move(peers),
	});
	if (history) {
		refreshHistory(history);
	}
	save();
	return id;
}

void LocalChatFilters::rename(int id, QString title) {
	if (auto filter = lookupMutable(id)) {
		if (filter->title != title) {
			filter->title = std::move(title);
			save();
		}
	}
}

void LocalChatFilters::remove(int id) {
	const auto i = ranges::find(_list, id, &LocalChatFilter::id);
	if (i == end(_list)) {
		return;
	}
	const auto runtimeId = LocalChatFilterRuntimeId(id);
	const auto j = _chatsLists.find(runtimeId);
	if (j != end(_chatsLists)) {
		const auto list = j->second.get();
		auto entries = std::vector<not_null<Dialogs::Entry*>>();
		entries.reserve(list->indexed()->all().size());
		for (const auto &row : list->indexed()->all()) {
			entries.push_back(row->entry());
		}
		for (const auto &entry : entries) {
			entry->removeFromChatList(runtimeId, list);
		}
	}
	auto histories = std::vector<not_null<History*>>();
	for (const auto &peerId : i->peers) {
		if (const auto history = _owner->historyLoaded(peerId)) {
			histories.push_back(history);
		}
	}
	_list.erase(i);
	for (const auto history : histories) {
		refreshHistory(history);
	}
	save();
}

void LocalChatFilters::reorder(int oldPosition, int newPosition) {
	if (oldPosition == newPosition) {
		return;
	}
	Expects(oldPosition >= 0 && oldPosition < _list.size());
	Expects(newPosition >= 0 && newPosition < _list.size());

	base::reorder(_list, oldPosition, newPosition);
	save();
}

void LocalChatFilters::savePinnedOrder(FilterId id) {
	Expects(IsLocalChatFilterListId(id));

	const auto peers = pinnedPeers(id);
	if (!peers) {
		return;
	}
	peers->clear();
	for (const auto &key : chatsList(id)->pinned()->order()) {
		if (const auto history = key.history()) {
			peers->push_back(history->peer->id);
		}
	}
	save(false);
}

bool LocalChatFilters::contains(
		int id,
		not_null<History*> history) const {
	if (const auto filter = lookup(id)) {
		return HasPeer(*filter, history->peer->id);
	}
	return false;
}

bool LocalChatFilters::containsRuntime(
		FilterId id,
		not_null<History*> history) const {
	return IsLocalChatFilterRuntimeId(id)
		&& contains(LocalChatFilterIdFromRuntime(id), history);
}

void LocalChatFilters::setContains(
		int id,
		not_null<History*> history,
		bool add) {
	const auto filter = lookupMutable(id);
	if (!filter) {
		return;
	}
	const auto peerId = history->peer->id;
	const auto had = HasPeer(*filter, peerId);
	if (had == add) {
		return;
	}
	if (add) {
		filter->peers.push_back(peerId);
	} else {
		filter->peers.erase(
			ranges::remove(filter->peers, peerId),
			end(filter->peers));
	}
	refreshHistory(history);
	save();
}

void LocalChatFilters::refreshHistory(not_null<History*> history) {
	if (history->inChatList()) {
		_owner->refreshChatListEntry(history);
	}
	refreshPinnedOrder(history);
}

void LocalChatFilters::refreshPinnedOrder(not_null<History*> history) {
	if (_pinnedOrderChangeDepth) {
		return;
	}
	const auto peerId = history->peer->id;
	if (HasPeer(_allPinnedPeers, peerId)) {
		applyPinnedOrder(LocalChatFilterAllRuntimeId());
	}
	for (const auto &filter : _list) {
		if (HasPeer(filter.pinnedPeers, peerId)) {
			applyPinnedOrder(LocalChatFilterRuntimeId(filter.id));
		}
	}
}

std::vector<PeerId> *LocalChatFilters::pinnedPeers(FilterId id) {
	Expects(IsLocalChatFilterListId(id));

	if (id == LocalChatFilterAllRuntimeId()) {
		return &_allPinnedPeers;
	}
	if (const auto filter = lookupMutable(LocalChatFilterIdFromRuntime(id))) {
		return &filter->pinnedPeers;
	}
	return nullptr;
}

void LocalChatFilters::applyPinnedOrder(FilterId id) {
	Expects(IsLocalChatFilterListId(id));

	const auto peers = pinnedPeers(id);
	if (!peers) {
		return;
	}
	auto histories = std::vector<not_null<History*>>();
	histories.reserve(peers->size());
	for (const auto &peerId : *peers) {
		if (const auto history = _owner->historyLoaded(peerId)) {
			if (history->inChatList(id)) {
				histories.push_back(history);
			}
		}
	}
	++_pinnedOrderChangeDepth;
	const auto guard = gsl::finally([&] {
		--_pinnedOrderChangeDepth;
	});
	chatsList(id)->pinned()->applyList(histories);
}

void LocalChatFilters::save(bool notify) {
	_owner->session().settings().setLocalChatFilters(_list);
	_owner->session().settings().setLocalChatFilterAllPinnedPeers(
		_allPinnedPeers);
	_owner->session().settings().setNextLocalChatFilterId(_nextId);
	_owner->session().saveSettingsDelayed();
	if (notify) {
		_changed.fire({});
	}
}

} // namespace Data
