// MODULE `websync`: the "Sync with a computer or phone" part of the Cloud tab. It drives the console's Syncthing FROM THE PHONE, with no
// terminal and no second web page: the page shows this console's code, takes the code of the other device, and shares the folders that
// matter (saves, save states, screenshots).
//
//   GET  /files/sync                JSON: installed, running, myid, devices [{id,name,connected}], folders [{preset,label,path,devices:[id]}]
//   POST /files/sync/power          {"on":true|false}              starts / stops the service (and keeps the choice for the next boot)
//   POST /files/sync/device         {"id":"AAAAAAA-...","name":".."}  adds the other device
//   POST /files/sync/device/remove  {"id":".."}
//   POST /files/sync/share          {"preset":"saves"|"states"|"screenshots","device":"<id>"}   shares that folder with the device
//   POST /files/sync/unshare        {"preset":".."}                stops sharing that folder (the files stay where they are)
//
// Safety: the same PIN / paired-phone check as every other route. Syncthing is talked to through its local REST interface with `curl`
// (no shell: arguments one by one); the API key is read from Syncthing's own config.xml and goes to curl through its standard input, so it
// never shows in an argument list, and the page never gets it back. Only three fixed folders can be shared: a path is never taken from
// the page. Paths and programs come from ZF_SYNC_* variables in tests, never from /storage.
//   ZF_SYNC_CONF (config.xml), ZF_SYNC_URL (REST base), ZF_SYNC_ROOT (/storage), ZF_SYNCTHING (the binary), ZF_CURL, ZF_SYNC_SERVICE
//   (a program called with `start` or `stop`, instead of the real service commands)
#pragma once
#include <string>

namespace zf {
namespace websync {

std::string conf_path();
std::string base_url();
std::string root_path();
std::string syncthing_path();
std::string curl_path();

/// The content of <apikey> in Syncthing's config.xml, "" when there is none.
std::string api_key_from(const std::string& xml);
/// A device code: 8 groups of 7 letters A-Z / digits 2-7 joined by dashes. Spaces are dropped and letters upper-cased first; false when it is not one.
bool normalise_device_id(const std::string& in, std::string& out);

struct Preset {
  const char* key;    // "saves"
  const char* label;  // shown in Syncthing
  const char* rel;    // path below the root
  bool send_only;     // the console only sends (screenshots)
};
/// One of the three fixed folders, nullptr for anything else.
const Preset* find_preset(const std::string& key);

struct Reply {
  int status = 0;  // 0: Syncthing did not answer (not running, no key, no curl)
  std::string body;
};
/// One call to Syncthing's REST interface.
Reply rest(const std::string& method, const std::string& path, const std::string& body);

/// The markup that goes inside the Cloud tab's section (the module's own script wires it up).
std::string section_html();

}  // namespace websync
}  // namespace zf
