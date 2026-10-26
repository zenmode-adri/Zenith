// `menu.scraper` (ES: SCRAPER): options, systems, account and the progress / summary screen of a scrape. The engine is in
// scraper_run.hpp; this file is only the screens.
//
//   menu.scraper           the main menu: games to scrape, recently scraped filter, systems, settings, account, START
//   menu.scraper.systems   one toggle per system ScreenScraper knows (those left out are stored in `ZfScraperSkip`)
//   menu.scraper.options   what to download (ES's keys: ScrapperImageSrc, ScrapeVideos...) and image size
//   menu.scraper.account   user / password (ES's ScreenScraperUser / ScreenScraperPass) and a test of the account
//   menu.scraper.run       progress while scraping, summary (and the errors) when it ends
#pragma once
#include <memory>

#include "app.hpp"
#include "scraper_run.hpp"

namespace zf {

/// Options of a run started from the menu: ES's settings plus the filter and the systems chosen in `menu.scraper`.
scraper::Options scraper_run_options(Ctx& cx);
/// The progress screen for an already prepared set of options (tests / PNG review lower the pacing).
std::unique_ptr<Screen> make_scraper_run_screen(Ctx& cx, scraper::Options o);

}  // namespace zf
