/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/widgets/chat_filters_tabs_strip.h"

#include "api/api_chat_filters_remove_manager.h"
#include "base/algorithm.h"
#include "boxes/choose_filter_box.h"
#include "boxes/filters/edit_filter_box.h"
#include "boxes/premium_limits_box.h"
#include "core/application.h"
#include "core/shortcuts.h"
#include "core/ui_integration.h"
#include "data/data_chat_filters.h"
#include "data/data_local_chat_filters.h"
#include "data/data_peer_values.h" // Data::AmPremiumValue.
#include "data/data_premium_limits.h"
#include "data/data_session.h"
#include "data/data_unread_value.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/sections/settings_folders.h"
#include "menu/menu_checked_action.h"
#include "ui/widgets/menu/menu_action.h"
#include "ui/layers/generic_box.h"
#include "ui/power_saving.h"
#include "ui/ui_utility.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/chat_filters_tabs_slider_reorder.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/slide_wrap.h"
#include "window/window_controller.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_dialogs.h" // dialogsSearchTabs
#include "styles/style_chat_helpers.h"
#include "styles/style_media_player.h" // mediaPlayerMenuCheck
#include "styles/style_menu_icons.h"

#include <QScrollBar>

namespace Ui {
namespace {

struct State final {
	Ui::Animations::Simple animation;
	std::optional<FilterId> lastFilterId = std::nullopt;
	rpl::lifetime rebuildLifetime;
	rpl::lifetime reorderLifetime;
	base::unique_qptr<Ui::PopupMenu> menu;

	Api::RemoveComplexChatFilter removeApi;
	bool waitingSuggested = false;

	std::unique_ptr<Ui::ChatsFiltersTabsReorder> reorder;
	bool ignoreRefresh = false;
	bool lastLocalShown = false;
};

constexpr auto kMaxLocalChatFilterTitleLength = 32;

void EditLocalChatFilterNameBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		int id) {
	const auto create = (id == 0);
	const auto filter = create
		? nullptr
		: controller->session().data().localChatFilters().lookup(id);
	box->setTitle(create
		? tr::lng_filters_local_create()
		: tr::lng_filters_local_rename());

	const auto initial = filter
		? filter->title
		: tr::lng_filters_local_default(tr::now);
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		tr::lng_filters_local_name(),
		initial));
	field->setMaxLength(kMaxLocalChatFilterTitleLength);
	field->selectAll();
	box->setFocusCallback([=] {
		field->setFocusFast();
	});
	const auto submit = [=] {
		auto title = base::CleanAndSimplify(field->getLastText());
		if (title.isEmpty()) {
			title = tr::lng_filters_local_default(tr::now);
		}
		box->closeBox();
		auto &local = controller->session().data().localChatFilters();
		if (create) {
			const auto created = local.create(std::move(title));
			controller->setLocalChatFiltersShown(true);
			controller->setActiveLocalChatFilter(
				Data::LocalChatFilterRuntimeId(created));
		} else {
			local.rename(id, std::move(title));
		}
	};
	field->submits() | rpl::on_next(submit, field->lifetime());
	if (create) {
		box->addButton(tr::lng_filters_create_button(), submit);
	} else {
		box->addButton(tr::lng_settings_save(), submit);
	}
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void AddFilterSourceActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller) {
	const auto local = controller->localChatFiltersShownCurrent();
	::Menu::AddCheckedAction(
		menu,
		tr::lng_filters_source_official(tr::now),
		[=] { controller->setLocalChatFiltersShown(false); },
		&st::menuIconShowInFolder,
		!local);
	::Menu::AddCheckedAction(
		menu,
		tr::lng_filters_source_local(tr::now),
		[=] { controller->setLocalChatFiltersShown(true); },
		&st::menuIconAddToFolder,
		local);
	menu->addSeparator();
}

void ShowLocalMenu(
		not_null<Ui::RpWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<State*> state,
		int index) {
	const auto session = &controller->session();
	const auto &list = session->data().localChatFilters().list();
	const auto localIndex = index - 1;
	const auto hasLocalFilter = (localIndex >= 0)
		&& (localIndex < int(list.size()));
	const auto localId = hasLocalFilter ? list[localIndex].id : 0;
	const auto runtimeId = localId
		? Data::LocalChatFilterRuntimeId(localId)
		: FilterId();

	state->menu = base::make_unique_q<Ui::PopupMenu>(
		parent,
		st::popupMenuWithIcons);
	AddFilterSourceActions(state->menu.get(), controller);
	const auto addAction = Ui::Menu::CreateAddActionCallback(
		state->menu.get());

	if (localId) {
		addAction(
			tr::lng_filters_local_rename(tr::now),
			[=] {
				controller->show(Box(
					EditLocalChatFilterNameBox,
					controller,
					localId));
			},
			&st::menuIconEdit);
		Window::MenuAddMarkAsReadChatListAction(
			controller,
			[=] {
				return session->data().localChatFilters().chatsList(
					runtimeId);
			},
			addAction);
		addAction({
			.text = tr::lng_filters_local_delete(tr::now),
			.handler = [=] {
				auto callback = [=](Fn<void()> &&close) {
					if (controller->activeLocalChatFilterCurrent() == runtimeId) {
						controller->setActiveLocalChatFilter(0);
					}
					session->data().localChatFilters().remove(localId);
					close();
				};
				controller->show(
					Ui::MakeConfirmBox({
						tr::lng_filters_local_delete_sure(),
						std::move(callback)
					}),
					Ui::LayerOption::CloseOther);
			},
			.icon = &st::menuIconDeleteAttention,
			.isAttention = true,
		});
	} else {
		Window::MenuAddMarkAsReadChatListAction(
			controller,
			[=] {
				return session->data().localChatFilters().chatsList(
					Data::LocalChatFilterAllRuntimeId());
			},
			addAction);
	}
	addAction(
		tr::lng_filters_local_create(tr::now),
		[=] {
			controller->show(Box(
				EditLocalChatFilterNameBox,
				controller,
				0));
		},
		&st::menuIconAddToFolder);

	state->menu->popup(QCursor::pos());
}

void ShowMenu(
		not_null<Ui::RpWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<State*> state,
		int index) {
	if (controller->localChatFiltersShownCurrent()) {
		ShowLocalMenu(parent, controller, state, index);
		return;
	}
	const auto session = &controller->session();

	auto id = FilterId(0);
	{
		const auto &list = session->data().chatsFilters().list();
		if (index < 0 || index >= list.size()) {
			return;
		}
		id = list[index].id();
	}
	state->menu = base::make_unique_q<Ui::PopupMenu>(
		parent,
		st::popupMenuWithIcons);
	const auto addAction = Ui::Menu::CreateAddActionCallback(
		state->menu.get());
	AddFilterSourceActions(state->menu.get(), controller);

	if (id) {
		addAction(
			tr::lng_filters_context_edit(tr::now),
			[=] { EditExistingFilter(controller, id); },
			&st::menuIconEdit);

		Window::MenuAddMarkAsReadChatListAction(
			controller,
			[=] { return session->data().chatsFilters().chatsList(id); },
			addAction);

		auto showRemoveBox = [=] {
			state->removeApi.request(base::make_weak(parent), controller, id);
		};
		addAction({
			.text = tr::lng_filters_context_remove(tr::now),
			.handler = std::move(showRemoveBox),
			.icon = &st::menuIconDeleteAttention,
			.isAttention = true,
		});
	} else {
		auto customUnreadState = [=] {
			return Data::MainListMapUnreadState(
				session,
				session->data().chatsList()->unreadState());
		};
		Window::MenuAddMarkAsReadChatListAction(
			controller,
			[=] { return session->data().chatsList(); },
			addAction,
			std::move(customUnreadState));

		auto openFiltersSettings = [=] {
			const auto filters = &session->data().chatsFilters();
			if (filters->suggestedLoaded()) {
				controller->showSettings(Settings::FoldersId());
			} else if (!state->waitingSuggested) {
				state->waitingSuggested = true;
				filters->requestSuggested();
				filters->suggestedUpdated(
				) | rpl::take(1) | rpl::on_next([=] {
					controller->showSettings(Settings::FoldersId());
				}, parent->lifetime());
			}
		};
		addAction(
			tr::lng_filters_setup_menu(tr::now),
			std::move(openFiltersSettings),
			&st::menuIconEdit);
	}
	if (state->menu->empty()) {
		state->menu = nullptr;
		return;
	}
	state->menu->popup(QCursor::pos());
}

void ShowFiltersListMenu(
		not_null<Ui::RpWidget*> parent,
		not_null<Main::Session*> session,
		not_null<State*> state,
		int active,
		Fn<void(int)> changeActive) {
	const auto &list = session->data().chatsFilters().list();

	state->menu = base::make_unique_q<Ui::PopupMenu>(
		parent,
		st::popupMenuWithIcons);

	const auto reorderAll = session->user()->isPremium();
	const auto maxLimit = (reorderAll ? 1 : 0)
		+ Data::PremiumLimits(session).dialogFiltersCurrent();
	const auto premiumFrom = (reorderAll ? 0 : 1) + maxLimit;

	for (auto i = 0; i < list.size(); ++i) {
		const auto title = list[i].title();
		const auto text = title.text.empty()
			? tr::lng_filters_all_short(tr::now)
			: title.text.text;
		const auto callback = [=] {
			if (i != active) {
				changeActive(i);
			}
		};
		const auto icon = (i == active)
			? &st::mediaPlayerMenuCheck
			: nullptr;
		const auto action = Ui::Menu::CreateAction(
			state->menu->menu(),
			text,
			callback);
		auto item = base::make_unique_q<Ui::Menu::Action>(
			state->menu->menu(),
			state->menu->st().menu,
			action,
			icon,
			icon);
		action->setEnabled(i < premiumFrom);
		if (!title.text.empty()) {
			const auto context = Core::TextContext({
				.session = session,
				.repaint = [raw = item.get()] { raw->update(); },
				.customEmojiLoopLimit = title.isStatic ? -1 : 0,
			});
			item->setMarkedText(title.text, QString(), context);
		}
		state->menu->addAction(std::move(item));
	}
	session->data().chatsFilters().changed() | rpl::on_next([=] {
		state->menu->hideMenu();
	}, state->menu->lifetime());

	if (state->menu->empty()) {
		state->menu = nullptr;
		return;
	}
	state->menu->popup(QCursor::pos());
}

} // namespace

not_null<Ui::RpWidget*> AddChatFiltersTabsStrip(
		not_null<Ui::RpWidget*> parent,
		not_null<Main::Session*> session,
		Fn<void(FilterId)> choose,
		ChatHelpers::PauseReason pauseLevel,
		Window::SessionController *controller,
		bool trackActiveFilterAndUnreadAndReorder,
		bool handleKeyboardSwitch) {

	const auto wrap = Ui::CreateChild<Ui::SlideWrap<Ui::RpWidget>>(
		parent,
		object_ptr<Ui::RpWidget>(parent));
	if (!controller) {
		const auto window = Core::App().findWindow(parent);
		controller = window ? window->sessionController() : nullptr;
		if (!controller) {
			return wrap;
		}
	}
	const auto container = wrap->entity();
	const auto scroll = Ui::CreateChild<Ui::ScrollArea>(
		container,
		st::dialogsTabsScroll,
		true);
	const auto slider = scroll->setOwnedWidget(
		object_ptr<Ui::ChatsFiltersTabs>(
			parent,
			trackActiveFilterAndUnreadAndReorder
				? st::dialogsSearchTabs
				: st::chatsFiltersTabs));
	const auto state = wrap->lifetime().make_state<State>();
	const auto localShown = [=] {
		return trackActiveFilterAndUnreadAndReorder
			&& controller->localChatFiltersShownCurrent();
	};
	const auto filterIdByIndex = [=](int index) {
		if (localShown()) {
			const auto &list = session->data().localChatFilters().list();
			Assert(index >= 0 && index <= int(list.size()));
			return index
				? Data::LocalChatFilterRuntimeId(list[index - 1].id)
				: FilterId();
		}
		const auto &list = session->data().chatsFilters().list();
		Assert(index >= 0 && index < int(list.size()));
		return list[index].id();
	};
	const auto reassignUnreadValue = [=] {
		state->reorderLifetime.destroy();
		const auto count = localShown()
			? int(session->data().localChatFilters().list().size()) + 1
			: int(session->data().chatsFilters().list().size());
		auto includeMuted = Data::IncludeMutedCounterFoldersValue();
		for (auto i = 0; i < count; i++) {
			const auto filterId = filterIdByIndex(i);
			const auto unreadFilterId = localShown()
				? Data::LocalChatFilterListId(filterId)
				: filterId;
			rpl::combine(
				Data::UnreadStateValue(session, unreadFilterId),
				rpl::duplicate(includeMuted)
			) | rpl::on_next([=](
					const Dialogs::UnreadState &state,
					bool includeMuted) {
				const auto chats = state.chats;
				const auto chatsMuted = state.chatsMuted;
				const auto muted = (chatsMuted + state.marksMuted);
				const auto count = (chats + state.marks)
					- (includeMuted ? 0 : muted);
				const auto isMuted = includeMuted && (count == muted);
				slider->setUnreadCount(i, count, isMuted);
				slider->fitWidthToSections();
			}, state->reorderLifetime);
		}
	};
	if (trackActiveFilterAndUnreadAndReorder) {
		using Reorder = Ui::ChatsFiltersTabsReorder;
		state->reorder = std::make_unique<Reorder>(slider, scroll);
		const auto applyReorder = [=](
				int oldPosition,
				int newPosition) {
			if (newPosition == oldPosition) {
				return;
			}
			if (localShown()) {
				if (!oldPosition || !newPosition) {
					return;
				}
				session->data().localChatFilters().reorder(
					oldPosition - 1,
					newPosition - 1);
				return;
			}

			const auto filters = &session->data().chatsFilters();
			const auto &list = filters->list();
			if (!session->user()->isPremium()) {
				if (list[0].id() != FilterId()) {
					filters->moveAllToFront();
				}
			}
			Assert(oldPosition >= 0 && oldPosition < list.size());
			Assert(newPosition >= 0 && newPosition < list.size());

			auto order = ranges::views::all(
				list
			) | ranges::views::transform(
				&Data::ChatFilter::id
			) | ranges::to_vector;
			base::reorder(order, oldPosition, newPosition);

			state->ignoreRefresh = true;
			filters->saveOrder(order);
			state->ignoreRefresh = false;
		};

		state->reorder->updates(
		) | rpl::on_next([=](const Reorder::Single &data) {
			if (data.state == Reorder::State::Started) {
				slider->setReordering(slider->reordering() + 1);
			} else {
				Ui::PostponeCall(slider, [=] {
					slider->setReordering(slider->reordering() - 1);
				});
				if (data.state == Reorder::State::Applied) {
					applyReorder(data.oldPosition, data.newPosition);
					reassignUnreadValue();
				}
			}
		}, slider->lifetime());

		SetupFilterDragAndDrop(
			slider,
			session,
			[=](QPoint pos) -> std::optional<FilterId> {
				const auto local = slider->mapFromGlobal(pos);
				const auto x = local.x();
				const auto count = slider->sectionsCount();
				for (auto i = 0; i < count; ++i) {
					const auto left = slider->lookupSectionLeft(i);
					const auto right = (i + 1 < count)
						? slider->lookupSectionLeft(i + 1)
						: slider->width();
					if (x >= left && x < right) {
						return filterIdByIndex(i);
					}
				}
				return std::nullopt;
			},
			[=] { return state->lastFilterId.value_or(FilterId()); },
			[=](FilterId id) {
				const auto count = slider->sectionsCount();
				for (auto i = 0; i < count; i++) {
					if (filterIdByIndex(i) == id) {
						slider->selectSection(i);
						return;
					}
				}
				slider->selectSection(-1);
			});
	}
	wrap->toggle(false, anim::type::instant);
	scroll->setCustomWheelProcess([=](not_null<QWheelEvent*> e) {
		const auto pixelDelta = e->pixelDelta();
		const auto angleDelta = e->angleDelta();
		if (std::abs(pixelDelta.x()) + std::abs(angleDelta.x())) {
			return false;
		}
		const auto bar = scroll->horizontalScrollBar();
		const auto y = pixelDelta.y() ? pixelDelta.y() : angleDelta.y();
		bar->setValue(bar->value() - y);
		return true;
	});

	const auto scrollToIndex = [=](int index, anim::type type) {
		const auto to = index
			? (slider->centerOfSection(index) - scroll->width() / 2)
			: 0;
		const auto bar = scroll->horizontalScrollBar();
		state->animation.stop();
		if (type == anim::type::instant) {
			bar->setValue(to);
		} else {
			state->animation.start(
				[=](float64 v) { bar->setValue(v); },
				bar->value(),
				std::min(to, bar->maximum()),
				st::defaultTabsSlider.duration);
		}
	};

	const auto applyFilterId = [=](FilterId id) {
		if (slider->reordering()) {
			return;
		}
		choose(id);
	};

	const auto rebuild = [=] {
		const auto local = localShown();
		const auto &list = session->data().chatsFilters().list();
		const auto &localList = session->data().localChatFilters().list();
		const auto count = local ? int(localList.size()) + 1 : int(list.size());
		if ((!local && count <= 1 && !slider->width())
			|| state->ignoreRefresh) {
			return;
		}
		const auto context = Core::TextContext({ .session = session });
		const auto paused = [=] {
			return On(PowerSaving::kEmojiChat)
				|| controller->isGifPausedAtLeastFor(pauseLevel);
		};
		auto sections = std::vector<TextWithEntities>();
		sections.reserve(count);
		if (local) {
			sections.push_back({ tr::lng_filters_all_short(tr::now) });
			for (const auto &filter : localList) {
				sections.push_back(TextWithEntities{ filter.title });
			}
		} else {
			sections = ranges::views::all(
				list
			) | ranges::views::transform([](const Data::ChatFilter &filter) {
				auto title = filter.title();
				return title.text.empty()
					? TextWithEntities{ tr::lng_filters_all_short(tr::now) }
					: title.isStatic
					? Data::ForceCustomEmojiStatic(title.text)
					: title.text;
			}) | ranges::to_vector;
		}
		const auto sectionsChanged = slider->setSectionsAndCheckChanged(
			std::move(sections),
			context,
			paused);
		const auto sourceChanged = (state->lastLocalShown != local);
		state->lastLocalShown = local;
		if (!sectionsChanged && !sourceChanged) {
			return;
		}
		state->rebuildLifetime.destroy();
		slider->fitWidthToSections();
		{
			const auto reorderAll = local || session->user()->isPremium();
			const auto maxLimit = local
				? count
				: (reorderAll ? 1 : 0)
					+ Data::PremiumLimits(session).dialogFiltersCurrent();
			const auto premiumFrom = local
				? count
				: (reorderAll ? 0 : 1) + maxLimit;
			slider->setLockedFrom((premiumFrom >= count) ? 0 : premiumFrom);
			if (!local) {
				slider->lockedClicked() | rpl::on_next([=] {
					controller->show(Box(
						FiltersLimitBox,
						session,
						std::nullopt));
				}, state->rebuildLifetime);
			}
			if (state->reorder) {
				state->reorder->cancel();
				state->reorder->clearPinnedIntervals();
				if (local || !reorderAll) {
					state->reorder->addPinnedInterval(0, 1);
				}
				if (!local) {
					state->reorder->addPinnedInterval(
						premiumFrom,
						std::max(1, int(list.size()) - maxLimit));
				}
			}
		}
		if (trackActiveFilterAndUnreadAndReorder) {
			reassignUnreadValue();
		}
		[&] {
			const auto currentId = trackActiveFilterAndUnreadAndReorder
				? (local
					? controller->activeLocalChatFilterCurrent()
					: controller->activeChatsFilterCurrent())
				: filterIdByIndex(0);
			const auto lastFitsSource = state->lastFilterId
				&& (Data::IsLocalChatFilterRuntimeId(*state->lastFilterId)
					== local);
			const auto lookingId = lastFitsSource
				? *state->lastFilterId
				: currentId;
			for (auto i = 0; i < count; i++) {
				const auto id = filterIdByIndex(i);
				if (id == lookingId) {
					const auto wasLast = !!state->lastFilterId;
					state->lastFilterId = id;
					slider->setActiveSectionFast(i);
					scrollToIndex(
						i,
						wasLast ? anim::type::normal : anim::type::instant);
					if (wasLast || !trackActiveFilterAndUnreadAndReorder) {
						applyFilterId(id);
					}
					return;
				}
			}
			if (count) {
				const auto index = 0;
				const auto id = filterIdByIndex(index);
				state->lastFilterId = id;
				slider->setActiveSectionFast(index);
				scrollToIndex(index, anim::type::instant);
				applyFilterId(id);
			}
		}();
		if (trackActiveFilterAndUnreadAndReorder) {
			const auto activateId = [=](FilterId id) {
				for (auto i = 0; i < slider->sectionsCount(); ++i) {
					if (filterIdByIndex(i) == id) {
						slider->setActiveSection(i);
						scrollToIndex(i, anim::type::normal);
						break;
					}
				}
				state->reorder->finishReordering();
			};
			controller->activeChatsFilter(
			) | rpl::on_next([=](FilterId id) {
				if (!localShown()) {
					activateId(id);
				}
			}, state->rebuildLifetime);
			controller->activeLocalChatFilter(
			) | rpl::on_next([=](FilterId id) {
				if (localShown()) {
					activateId(id);
				}
			}, state->rebuildLifetime);
		}
		rpl::single(-1) | rpl::then(
			slider->sectionActivated()
		) | rpl::combine_previous(
		) | rpl::on_next([=](int was, int index) {
			if (slider->reordering()) {
				return;
			}
			const auto id = filterIdByIndex(index);
			if (was != index) {
				state->lastFilterId = id;
				scrollToIndex(index, anim::type::normal);
			}
			applyFilterId(id);
		}, state->rebuildLifetime);
		slider->contextMenuRequested() | rpl::on_next([=](int index) {
			if (trackActiveFilterAndUnreadAndReorder) {
				ShowMenu(wrap, controller, state, index);
			} else {
				ShowFiltersListMenu(
					wrap,
					session,
					state,
					slider->activeSection(),
					[=](int i) { slider->setActiveSection(i); });
			}
		}, state->rebuildLifetime);
		wrap->toggle(local || (count > 1), anim::type::instant);

		if (state->reorder) {
			state->reorder->start();
		}
	};
	rpl::merge(
		session->data().chatsFilters().changed(),
		session->data().localChatFilters().changed(),
		controller->localChatFiltersShown() | rpl::to_empty,
		Data::AmPremiumValue(session) | rpl::to_empty
	) | rpl::on_next(rebuild, wrap->lifetime());
	rebuild();

	session->data().chatsFilters().isChatlistChanged(
	) | rpl::on_next([=](FilterId id) {
		if (!id || !state->lastFilterId || (id != state->lastFilterId)) {
			return;
		}
		for (const auto &filter : session->data().chatsFilters().list()) {
			if (filter.id() == id) {
				applyFilterId(id);
				return;
			}
		}
	}, wrap->lifetime());

	rpl::combine(
		parent->widthValue() | rpl::filter(rpl::mappers::_1 > 0),
		slider->heightValue() | rpl::filter(rpl::mappers::_1 > 0)
	) | rpl::on_next([=](int w, int h) {
		scroll->resize(w, h);
		container->resize(w, h);
		wrap->resize(w, h);
	}, wrap->lifetime());

	if (handleKeyboardSwitch) {
		Shortcuts::ChatSwitchRequests(
		) | rpl::filter([=](const Shortcuts::ChatSwitchRequest &request) {
			return wrap->toggled()
				&& ((request.action == Qt::Key_Tab)
					|| (request.action == Qt::Key_Backtab));
		}) | rpl::on_next([=](const Shortcuts::ChatSwitchRequest &request) {
			const auto count = slider->sectionsCount();
			const auto locked = slider->lockedFrom();
			const auto limit = locked ? locked : count;
			if (limit <= 1) {
				return;
			}
			const auto back = (request.action == Qt::Key_Backtab);
			const auto current = std::min(slider->activeSection(), limit - 1);
			const auto next = (current + (back ? -1 : 1) + limit) % limit;
			slider->setActiveSection(next);
		}, wrap->lifetime());
	}

	return wrap;
}

} // namespace Ui
