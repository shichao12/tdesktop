/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_message_keyword_blacklist.h"

#include "base/weak_ptr.h"
#include "base/flat_map.h"
#include "data/data_message_keyword_blacklist.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "ui/controls/sub_tabs.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtGui/QCursor>

namespace Window {
namespace {

constexpr auto kMaxMessageBlacklistEntryLength = 128;

enum class BlacklistEntryType {
	Keyword,
	Link,
};

enum class BlacklistEntryFilter {
	All,
	Keyword,
	Link,
};

[[nodiscard]] QString EntryFilterId(BlacklistEntryFilter filter) {
	switch (filter) {
	case BlacklistEntryFilter::All:
		return u"all"_q;
	case BlacklistEntryFilter::Keyword:
		return u"keyword"_q;
	case BlacklistEntryFilter::Link:
		return u"link"_q;
	}
	Unexpected("Unknown blacklist entry filter.");
}

[[nodiscard]] BlacklistEntryFilter EntryFilterFromId(const QString &id) {
	if (id == EntryFilterId(BlacklistEntryFilter::Keyword)) {
		return BlacklistEntryFilter::Keyword;
	} else if (id == EntryFilterId(BlacklistEntryFilter::Link)) {
		return BlacklistEntryFilter::Link;
	}
	return BlacklistEntryFilter::All;
}

[[nodiscard]] std::vector<QString> CurrentEntries(
		not_null<SessionController*> controller,
		PeerData *peer,
		BlacklistEntryType type) {
	auto &blacklist = controller->session().data().messageKeywordBlacklist();
	if (type == BlacklistEntryType::Link) {
		const auto &links = blacklist.commonLinks();
		return { begin(links), end(links) };
	}
	const auto &entries = peer
		? blacklist.channelKeywords(peer->id)
		: blacklist.commonKeywords();
	return { begin(entries), end(entries) };
}

void SaveEntries(
		not_null<SessionController*> controller,
		PeerData *peer,
		std::vector<QString> entries,
		BlacklistEntryType type) {
	auto &blacklist = controller->session().data().messageKeywordBlacklist();
	if (type == BlacklistEntryType::Link) {
		blacklist.setCommonLinks(std::move(entries));
		return;
	}
	if (peer) {
		blacklist.setChannelKeywords(peer->id, std::move(entries));
	} else {
		blacklist.setCommonKeywords(std::move(entries));
	}
}

[[nodiscard]] rpl::producer<QString> AddEntryText(BlacklistEntryType type) {
	return (type == BlacklistEntryType::Link)
		? tr::lng_message_blacklist_add_link()
		: tr::lng_message_blacklist_add_keyword();
}

[[nodiscard]] rpl::producer<QString> EntryTypeText(BlacklistEntryType type) {
	return (type == BlacklistEntryType::Link)
		? tr::lng_message_blacklist_type_link()
		: tr::lng_message_blacklist_type_text();
}

[[nodiscard]] rpl::producer<QString> DeleteEntryText(BlacklistEntryType type) {
	return (type == BlacklistEntryType::Link)
		? tr::lng_message_blacklist_delete_link()
		: tr::lng_message_blacklist_delete_keyword();
}

[[nodiscard]] rpl::producer<QString> NoEntriesText(BlacklistEntryType type) {
	return (type == BlacklistEntryType::Link)
		? tr::lng_message_blacklist_no_links()
		: tr::lng_message_blacklist_no_keywords();
}

[[nodiscard]] rpl::producer<QString> EntryPlaceholder(
		BlacklistEntryType type) {
	return (type == BlacklistEntryType::Link)
		? tr::lng_message_blacklist_link_placeholder()
		: tr::lng_message_blacklist_keyword_placeholder();
}

[[nodiscard]] QString ItemPreview(not_null<HistoryItem*> item) {
	const auto text = item->originalText().text.simplified();
	return text.isEmpty()
		? tr::lng_message_blacklist_open(tr::now)
		: text;
}

void OpenItem(
		not_null<SessionController*> controller,
		FullMsgId itemId,
		base::weak_qptr<Ui::GenericBox> box) {
	const auto item = controller->session().data().message(itemId);
	if (!item) {
		if (const auto strong = box.get()) {
			strong->closeBox();
		}
		return;
	}
	auto &blacklist = item->history()->owner().messageKeywordBlacklist();
	blacklist.setExpanded(item, true);
	if (const auto strong = box.get()) {
		strong->closeBox();
	}
	const auto weak = base::make_weak(controller);
	crl::on_main(weak, [=] {
		if (const auto item = controller->session().data().message(itemId)) {
			controller->showMessage(item, SectionShow::Way::Forward);
		}
	});
}

void ShowAddEntryBox(
		base::weak_qptr<Ui::GenericBox> parent,
		not_null<SessionController*> controller,
		PeerData *peer,
		BlacklistEntryType initialType) {
	const auto strong = parent.get();
	if (!strong) {
		return;
	}
	strong->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		const auto type = box->lifetime().make_state<
			rpl::variable<BlacklistEntryType>>(initialType);
		const auto typeText = [=] {
			return type->value(
			) | rpl::map(EntryTypeText) | rpl::flatten_latest();
		};
		const auto addText = [=] {
			return type->value(
			) | rpl::map(AddEntryText) | rpl::flatten_latest();
		};
		box->setTitle(addText());
		if (!peer) {
			const auto button = Settings::AddButtonWithLabel(
				box->verticalLayout(),
				tr::lng_message_blacklist_entry_type(),
				typeText(),
				st::settingsButtonNoIcon);
			const auto menu = button->lifetime().make_state<
				base::unique_qptr<Ui::PopupMenu>>();
			button->addClickHandler([=] {
				*menu = base::make_unique_q<Ui::PopupMenu>(
					button,
					st::popupMenuWithIcons);
				(*menu)->addAction(
					tr::lng_message_blacklist_type_text(tr::now),
					[=] { *type = BlacklistEntryType::Keyword; });
				(*menu)->addAction(
					tr::lng_message_blacklist_type_link(tr::now),
					[=] { *type = BlacklistEntryType::Link; },
					&st::menuIconLink);
				(*menu)->popup(QCursor::pos());
			});
		}
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::SingleLine,
			type->value(
			) | rpl::map(EntryPlaceholder) | rpl::flatten_latest(),
			QString()));
		field->setMaxLength(kMaxMessageBlacklistEntryLength);
		box->setFocusCallback([=] {
			field->setFocusFast();
		});
		const auto submit = [=] {
			const auto entry = field->getLastText().trimmed();
			if (entry.isEmpty()) {
				return;
			}
			const auto entryType = type->current();
			auto entries = CurrentEntries(controller, peer, entryType);
			entries.push_back(entry);
			SaveEntries(controller, peer, std::move(entries), entryType);
			box->closeBox();
		};
		field->submits() | rpl::on_next(submit, field->lifetime());
		box->addButton(addText(), submit);
		box->addButton(
			tr::lng_message_blacklist_cancel(),
			[=] { box->closeBox(); });
	}));
}

void AddEntryButton(
		base::weak_qptr<Ui::GenericBox> box,
		not_null<Ui::VerticalLayout*> rows,
		not_null<SessionController*> controller,
		PeerData *peer,
		BlacklistEntryType type) {
	Settings::AddButtonWithIcon(
		rows,
		peer ? AddEntryText(type) : tr::lng_message_blacklist_add_entry(),
		st::settingsButtonActive,
		{ &st::menuIconAdd }
	)->addClickHandler([=] {
		ShowAddEntryBox(box, controller, peer, type);
	});
}

void FillEntries(
		base::weak_qptr<Ui::GenericBox> box,
		not_null<Ui::VerticalLayout*> rows,
		not_null<SessionController*> controller,
		PeerData *peer,
		BlacklistEntryType type) {
	if (!box) {
		return;
	}

	const auto entries = CurrentEntries(controller, peer, type);
	if (entries.empty()) {
		Ui::AddDividerText(rows, NoEntriesText(type));
		return;
	}

	for (const auto &entry : entries) {
		Settings::AddButtonWithLabel(
			rows,
			rpl::single(entry),
			DeleteEntryText(type),
			st::settingsButton,
			{ &st::menuIconDelete }
		)->addClickHandler([=] {
			auto entries = CurrentEntries(controller, peer, type);
			for (auto i = begin(entries); i != end(entries); ++i) {
				if (*i == entry) {
					entries.erase(i);
					break;
				}
			}
			SaveEntries(controller, peer, std::move(entries), type);
		});
	}
}

void FillKeywords(
		base::weak_qptr<Ui::GenericBox> box,
		not_null<Ui::VerticalLayout*> rows,
		not_null<SessionController*> controller,
		PeerData *peer,
		BlacklistEntryFilter filter) {
	if (!box) {
		return;
	}
	rows->clear();
	AddEntryButton(
		box,
		rows,
		controller,
		peer,
		(!peer && filter == BlacklistEntryFilter::Link)
			? BlacklistEntryType::Link
			: BlacklistEntryType::Keyword);
	if (!peer && filter != BlacklistEntryFilter::Link) {
		Ui::AddSubsectionTitle(
			rows,
			tr::lng_message_blacklist_common_keywords());
		FillEntries(box, rows, controller, peer, BlacklistEntryType::Keyword);
	}
	if (!peer && filter != BlacklistEntryFilter::Keyword) {
		Ui::AddSubsectionTitle(
			rows,
			tr::lng_message_blacklist_common_links());
		FillEntries(box, rows, controller, peer, BlacklistEntryType::Link);
	}
	if (peer) {
		FillEntries(box, rows, controller, peer, BlacklistEntryType::Keyword);
	}
}

void FillItems(
		base::weak_qptr<Ui::GenericBox> box,
		not_null<Ui::VerticalLayout*> rows,
		not_null<SessionController*> controller,
		PeerData *peer) {
	if (!box) {
		return;
	}
	rows->clear();

	auto &blacklist = controller->session().data().messageKeywordBlacklist();
	const auto items = blacklist.matchedItems(peer);
	if (items.empty()) {
		Ui::AddDividerText(rows, tr::lng_message_blacklist_empty());
		return;
	}

	struct Group {
		not_null<PeerData*> peer;
		std::vector<FullMsgId> itemIds;
		std::vector<QString> previews;
	};
	auto groups = std::vector<Group>();
	auto groupIndex = base::flat_map<PeerId, int>();
	for (const auto item : items) {
		const auto itemPeer = item->history()->peer;
		const auto index = [&] {
			const auto i = groupIndex.find(itemPeer->id);
			if (i != end(groupIndex)) {
				return i->second;
			}
			const auto result = int(groups.size());
			groupIndex.emplace(itemPeer->id, result);
			groups.push_back({ itemPeer, {}, {} });
			return result;
		}();
		auto &group = groups[index];
		group.itemIds.push_back(item->fullId());
		group.previews.push_back(ItemPreview(item));
	}

	for (const auto &group : groups) {
		if (!peer) {
			Ui::AddSubsectionTitle(rows, rpl::single(group.peer->name()));
		}
		for (auto i = 0, count = int(group.itemIds.size()); i != count; ++i) {
			const auto itemId = group.itemIds[i];
			Settings::AddButtonWithIcon(
				rows,
				rpl::single(group.previews[i]),
				st::settingsButton,
				{ &st::menuIconShowInChat }
			)->addClickHandler([=] {
				OpenItem(controller, itemId, box);
			});
		}
	}
}

} // namespace

void ShowMessageKeywordBlacklistKeywordsBox(
		not_null<SessionController*> controller,
		PeerData *peer) {
	if (peer && !peer->asBroadcast()) {
		return;
	}
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(peer
			? tr::lng_message_blacklist_channel_keywords()
			: tr::lng_message_blacklist_manage_common());
		box->setMaxHeight(st::boxMaxListHeight);
		if (!peer) {
			box->setWidth(st::boxWideWidth);
			box->setMinHeight(st::boxMaxListHeight);
		}
		box->addButton(
			tr::lng_message_blacklist_close(),
			[=] { box->closeBox(); });

		const auto filter = box->lifetime().make_state<
			rpl::variable<BlacklistEntryFilter>>(BlacklistEntryFilter::All);
		if (!peer) {
			const auto tabs = box->addRow(
				object_ptr<Ui::SubTabs>(
					box,
					st::defaultSubTabs,
					Ui::SubTabsOptions{
						.selected = EntryFilterId(filter->current()),
						.centered = true,
					},
					std::vector<Ui::SubTabsTab>{
						{
							EntryFilterId(BlacklistEntryFilter::All),
							tr::lng_message_blacklist_filter_all(
								tr::now,
								tr::marked),
						},
						{
							EntryFilterId(BlacklistEntryFilter::Keyword),
							tr::lng_message_blacklist_type_text(
								tr::now,
								tr::marked),
						},
						{
							EntryFilterId(BlacklistEntryFilter::Link),
							tr::lng_message_blacklist_type_link(
								tr::now,
								tr::marked),
						},
					}),
				st::boxRowPadding);
			tabs->activated() | rpl::on_next([=](const QString &id) {
				tabs->setActiveTab(id);
				*filter = EntryFilterFromId(id);
			}, tabs->lifetime());
		}

		const auto rows = box->verticalLayout()->add(
			object_ptr<Ui::VerticalLayout>(box->verticalLayout()));
		const auto weakBox = base::make_weak(box);
		const auto weakRows = base::make_weak(rows);
		FillKeywords(weakBox, rows, controller, peer, filter->current());

		controller->session().data().messageKeywordBlacklist().changed(
		) | rpl::on_next([=] {
			if (const auto strongRows = weakRows.get()) {
				FillKeywords(
					weakBox,
					strongRows,
					controller,
					peer,
					filter->current());
			}
		}, box->lifetime());
		filter->changes(
		) | rpl::on_next([=](BlacklistEntryFilter value) {
			if (const auto strongRows = weakRows.get()) {
				FillKeywords(weakBox, strongRows, controller, peer, value);
			}
		}, box->lifetime());
	}));
}

void ShowMessageKeywordBlacklistItemsBox(
		not_null<SessionController*> controller,
		PeerData *peer) {
	if (peer && !peer->asBroadcast()) {
		return;
	}
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(peer
			? tr::lng_message_blacklist_view_channel()
			: tr::lng_message_blacklist_title());
		box->setMaxHeight(st::boxMaxListHeight);
		box->addButton(
			tr::lng_message_blacklist_close(),
			[=] { box->closeBox(); });

		const auto rows = box->verticalLayout()->add(
			object_ptr<Ui::VerticalLayout>(box->verticalLayout()));
		const auto weakBox = base::make_weak(box);
		const auto weakRows = base::make_weak(rows);
		FillItems(weakBox, rows, controller, peer);

		controller->session().data().messageKeywordBlacklist().changed(
		) | rpl::on_next([=] {
			if (const auto strongRows = weakRows.get()) {
				FillItems(weakBox, strongRows, controller, peer);
			}
		}, box->lifetime());
	}));
}

} // namespace Window
