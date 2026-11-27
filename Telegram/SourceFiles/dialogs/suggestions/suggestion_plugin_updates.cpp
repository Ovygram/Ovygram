#include "dialogs/suggestions/suggestion.h"

#include "dialogs/ui/dialogs_top_bar_suggestion_content.h"
#include "ovygram/i18n_rpl.h"
#include "ovygram/plugin_updates.h"
#include "settings/sections/settings_plugins.h"
#include "ui/text/text_utilities.h"
#include "window/window_session_controller.h"

namespace Dialogs::TopBarSuggestions {

Spec MakePluginUpdatesSpec() {
	return {
		.priority = Priority::PluginUpdates,
		.available = [](const Context &) {
			return Ovygram::PluginUpdateCount() > 0;
		},
		.activate = [](ActivateArgs args) {
			const auto content = args.context.ensureContent();
			const auto findController = args.context.findController;
			content->setRightIcon(TopBarSuggestionContent::RightIcon::Close);
			content->setLeadingWidget(nullptr);
			content->setClickedCallback([=] {
				findController()->showSettings(::Settings::PluginsId());
			});
			content->setHideCallback([] {
				Ovygram::DismissPluginUpdateNotification();
			});
			const auto update = [=] {
				content->setContent(
					TextWithEntities::Simple(Ovygram::Tr(
						u"Plugin updates"_q,
						u"Обновления плагинов"_q)),
					TextWithEntities::Simple(Ovygram::Tr(
						u"Updates available: %1. Click to open Plugins."_q,
						u"Доступно обновлений: %1. Нажмите, чтобы открыть плагины."_q)
						.arg(Ovygram::PluginUpdateCount())));
			};
			update();
			Ovygram::PluginUpdateChanges() | rpl::on_next(update, *args.lifetime);
			Ovygram::I18n::LanguageChanges() | rpl::on_next([=](const QString &) {
				update();
			}, *args.lifetime);
			args.done(content, [=] { content->prepareCollapseSnapshot(); });
		},
	};
}

} // namespace Dialogs::TopBarSuggestions
