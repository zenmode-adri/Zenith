// MODULE `webi18n` (web module of webtransfer): the page in Spanish when the phone or PC is in Spanish (navigator.language es-*;
// `?lang=es` / `?lang=en` in the address choose by hand).
//
// The page and its modules are written in English. This module only adds a script: a dictionary of every text the page shows
// (labels, buttons, messages, and the error messages the console sends back) plus patterns for the texts built with numbers or
// names. It translates the page once and then every text that appears later (a MutationObserver on the page), and the questions
// of `confirm()`. A text it does not know stays in English; names of games and files are never translated unless one is exactly
// a dictionary entry. Nothing on the console changes: no route, no setting. tests/test_webi18n.cpp checks that every English text
// of the page has its entry.
#include "webcore.hpp"

namespace zf {
namespace webtransfer {
namespace {

const char* const kJs = R"JS(
var q=location.search;if(/[?&]lang=en\b/.test(q)||(!/[?&]lang=es\b/.test(q)&&!/^es\b/i.test(navigator.language||"")))return;
document.documentElement.lang="es";
var D={
/* core page */
"Remote":"Remoto","PIN shown on the console":"PIN que muestra la consola","Continue":"Continuar",
"Remember this device? It will not need the PIN next time.":"¿Recordar este dispositivo? La próxima vez no pedirá el PIN.",
"Remember this device":"Recordar este dispositivo","Not now":"Ahora no","Upload":"Subir","Games":"Juegos","Screenshots":"Capturas",
"System":"Sistema","Files":"Archivos","Folder":"Carpeta","Or drop files or a folder here.":"O suelta aquí archivos o una carpeta.",
"Replace files that already exist":"Reemplazar los archivos que ya existan","Search":"Buscar","All loaded systems":"Todos los sistemas cargados",
"More":"Más","Close game":"Cerrar juego","Refresh":"Actualizar","Wrong PIN.":"PIN incorrecto.",
"Too many wrong PINs. Wait 30 seconds.":"Demasiados PIN incorrectos. Espera 30 segundos.",
"File transfer is switched off on the console.":"La transferencia de archivos está apagada en la consola.",
"This device is no longer paired. Enter the PIN again.":"Este dispositivo ya no está vinculado. Escribe el PIN otra vez.",
"Cannot reach the console.":"No se puede conectar con la consola.","Answer on the console...":"Responde en la consola...",
"This device is remembered.":"Este dispositivo queda recordado.",
"This browser cannot keep it. It works until you close the page.":"Este navegador no puede guardarlo. Funciona hasta que cierres la página.",
"The console did not allow it.":"La consola no lo ha permitido.",
"The console is asking about another device. Try again in a moment.":"La consola está preguntando por otro dispositivo. Prueba dentro de un momento.",
"Choose at least one file or a folder.":"Elige al menos un archivo o una carpeta.","Reconnecting…":"Reconectando…","done":"hecho",
"connection lost":"conexión perdida","out of step with the console":"desincronizado con la consola","Connection lost.":"Conexión perdida.",
"No games found.":"No se han encontrado juegos.","No screenshots.":"No hay capturas.","No game is running.":"No hay ningún juego en marcha.",
"Close the running game?":"¿Cerrar el juego en marcha?","Closing the game...":"Cerrando el juego...","Loading...":"Cargando...",
"This system could not be read yet.":"Este sistema aún no se ha podido leer.",
/* games: manage */
"Launch":"Lanzar","Find picture online":"Buscar imagen en internet","Searching on the console. The picture shows up here when it is done.":"Buscando en la consola. La imagen aparecerá aquí cuando termine.","Rename":"Renombrar","Change picture":"Cambiar imagen","Move to trash":"Mover a la papelera","Cancel":"Cancelar",
"Close":"Cerrar","Save":"Guardar","Trash":"Papelera","Restore":"Restaurar","Empty trash":"Vaciar papelera","File name":"Nombre del archivo",
"Title shown in the lists":"Título que se ve en las listas",
"The extension cannot change. Saves and save states follow the new file name.":"La extensión no puede cambiar. Las partidas y los estados guardados siguen al nuevo nombre.",
"Move to trash?":"¿Mover a la papelera?",
"The game file goes to the trash on the console. You can restore it from the Trash button.":"El archivo del juego va a la papelera de la consola. Puedes restaurarlo desde el botón Papelera.",
"Moved to the trash.":"Movido a la papelera.","Renamed.":"Renombrado.","Picture saved.":"Imagen guardada.","Not saved.":"No se ha guardado.",
"Choose a picture.":"Elige una imagen.","PNG, JPEG or WebP, up to 8 MB.":"PNG, JPEG o WebP, hasta 8 MB.",
"The picture is larger than 8 MB.":"La imagen pasa de 8 MB.","The trash is empty.":"La papelera está vacía.","Empty the trash?":"¿Vaciar la papelera?",
"Trash emptied.":"Papelera vaciada.","Working on the console...":"Trabajando en la consola...",
/* BIOS */
"All BIOS files are fine.":"Todos los archivos BIOS están bien.","Show all systems":"Mostrar todos los sistemas","missing":"falta",
"wrong file":"archivo incorrecto","there (zip, not checked)":"está (en ZIP, sin comprobar)","No systems with games.":"No hay sistemas con juegos.",
"This is not the expected file. Save it anyway?":"No es el archivo esperado. ¿Guardarlo de todos modos?",
/* saves */
"Saves":"Partidas","Back up now":"Hacer copia ahora","Backing up...":"Haciendo copia...","No backups yet.":"Aún no hay copias.",
"Upload a backup (.tar.gz)":"Subir una copia (.tar.gz)","Download":"Descargar","Downloading...":"Descargando...",
"Backup uploaded.":"Copia subida.","Choose a backup file first.":"Elige antes un archivo de copia.",
"The file name must look like saves-YYYYMMDD-HHMM.tar.gz.":"El nombre del archivo debe ser como saves-AAAAMMDD-HHMM.tar.gz.",
"Replace the saves on the console with this backup? A backup of the current saves is made first.":"¿Reemplazar las partidas de la consola por esta copia? Antes se hace una copia de las actuales.",
"Restoring...":"Restaurando...","Confirm on the console...":"Confirma en la consola...","Saves restored.":"Partidas restauradas.","Sending...":"Enviando...","A game is running.":"Hay un juego en marcha.",
"A game is running: restoring is off until you close it.":"Hay un juego en marcha: no se puede restaurar hasta que lo cierres.",
/* messages from the console */
"a backup with that name already exists":"ya existe una copia con ese nombre","a file with that name already exists":"ya existe un archivo con ese nombre",
"a file with that name exists now":"ahora existe un archivo con ese nombre","a game is running":"hay un juego en marcha","declined on the console":"rechazado en la consola",
"a launch was just requested":"se acaba de pedir lanzar un juego","another backup operation is running":"ya hay otra operación de copia en marcha",
"another device is asking":"otro dispositivo está preguntando","another transfer is running":"hay otra transferencia en marcha",
"another upload is running":"hay otra subida en marcha","bad request":"petición incorrecta","cannot create the file":"no se puede crear el archivo",
"cannot create the images folder":"no se puede crear la carpeta de imágenes","cannot create the trash folder":"no se puede crear la papelera",
"cannot open the file":"no se puede abrir el archivo","cannot replace that file":"no se puede reemplazar ese archivo",
"cannot use that folder":"no se puede usar esa carpeta","could not move the file to the trash":"no se pudo mover el archivo a la papelera",
"could not put the file back":"no se pudo devolver el archivo","could not rename the file":"no se pudo renombrar el archivo",
"could not save the file":"no se pudo guardar el archivo","could not save the picture":"no se pudo guardar la imagen","empty file":"archivo vacío",
"game management is not available":"la gestión de juegos no está disponible","invalid backup name":"nombre de copia no válido",
"invalid file name":"nombre de archivo no válido","invalid path":"ruta no válida","invalid title":"título no válido",
"launch is not available":"lanzar no está disponible","no such backup":"esa copia no existe","not a BIOS file of the list":"no es un BIOS de la lista",
"not a regular file":"no es un archivo normal","not allowed":"no permitido","not available":"no disponible",
"not enough free space":"no hay espacio libre suficiente","not found":"no encontrado","not the expected file":"no es el archivo esperado",
"only PNG, JPEG or WebP pictures are accepted":"solo se aceptan imágenes PNG, JPEG o WebP","request timeout":"la petición tardó demasiado",
"request too large":"petición demasiado grande","that BIOS file is already there":"ese BIOS ya está","that is not a picture":"eso no es una imagen",
"that is the current name":"ese es el nombre actual","the BIOS list of this system is not available":"la lista de BIOS de este sistema no está disponible",
"the ROM folder does not exist":"la carpeta de ROMs no existe","the console cannot ask now":"la consola no puede preguntar ahora",
"the file extension cannot change":"la extensión del archivo no puede cambiar","the file is not in the trash any more":"el archivo ya no está en la papelera",
"the file is not there":"el archivo no está","the file is too big":"el archivo es demasiado grande","the picture is larger than 8 MB":"la imagen pasa de 8 MB",
"the trash folder is not a folder":"la papelera no es una carpeta",
"the trash is on another file system than the game; nothing was moved":"la papelera está en otro disco que el juego; no se ha movido nada",
"this game cannot be changed from here":"este juego no se puede cambiar desde aquí","this system does not accept that file type":"este sistema no acepta ese tipo de archivo",
"too big for a BIOS file":"demasiado grande para un BIOS","too many wrong PINs, wait a little":"demasiados PIN incorrectos, espera un poco",
"unknown game":"juego desconocido","unknown item":"elemento desconocido","unknown system":"sistema desconocido","wrong PIN":"PIN incorrecto",
"upload interrupted":"subida interrumpida","the offset does not match what the console has":"lo enviado no coincide con lo que tiene la consola",
"write failed":"falló la escritura","empty slice":"trozo vacío","slice too big":"trozo demasiado grande",
"no picture":"no hay imagen","invalid id":"identificador no válido","invalid game id":"juego no válido","method not allowed":"método no permitido",
"offset and length go past the total":"el trozo pasa del tamaño total","offset and total are required":"faltan la posición y el tamaño",
"system and name are required":"faltan el sistema y el nombre","system and path are required":"faltan el sistema y la ruta",
"file is required":"falta el archivo","too long":"demasiado largo","could not make a token":"no se pudo crear la clave","no font":"sin tipografía"
};
var P=[
[/^Overall (\d+)% - file (\d+) of (\d+)$/,"Total $1 % · archivo $2 de $3"],
[/^Finished: (\d+) of (\d+) files sent\.$/,"Terminado: $1 de $2 archivos enviados."],
[/^Uploading (\d+)%$/,"Subiendo $1 %"],
[/^Folder: (.*) - accepts: (.*) - (\d+) games$/,"Carpeta: $1 · acepta: $2 · $3 juegos"],
[/^Folder: (.*) - accepts: (.*)$/,"Carpeta: $1 · acepta: $2"],
[/^Free space: (.*)$/,"Espacio libre: $1"],
[/^Backup done: (.*)$/,"Copia hecha: $1"],
[/^Launch (.*)\?$/,"¿Lanzar $1?"],
[/^Launching (.*)\.\.\.$/,"Lanzando $1..."],
[/^Sending (.*)\.\.\.$/,"Enviando $1..."],
[/^Restored (.*)\.$/,"Restaurado: $1."],
[/^Saved (.*), but it is not the expected file and may not work\.$/,"Guardado $1, pero no es el archivo esperado y puede no funcionar."],
[/^Saved (.*)\.$/,"Guardado $1."],
[/^Actions for (.*)$/,"Acciones de $1"],
[/^(\d+) games? will be deleted for good\. This cannot be undone\.$/,"Se borrarán para siempre $1 juegos. No se puede deshacer."],
[/^(.*) - (\d+) to fix$/,"$1 · $2 por arreglar"],
[/^(\d+) missing$/,"Faltan $1"],[/^1 wrong$/,"1 incorrecto"],[/^(\d+) wrong$/,"$1 incorrectos"],[/^(\d+) in zip files, not checked$/,"$1 en ZIP, sin comprobar"],
[/^All BIOS files are fine\. (.*)\.$/,"Todos los BIOS están bien. $1."],
[/^Error (\d+)$/,"Error $1"],
[/^This is not the expected file\. Save it anyway\?\n\(md5 ([0-9a-f]+)\)$/,"No es el archivo esperado. ¿Guardarlo de todos modos?\n(md5 $1)"]
];
function one(k){if(Object.prototype.hasOwnProperty.call(D,k))return D[k];
 for(var i=0;i<P.length;i++)if(P[i][0].test(k))return k.replace(P[i][0],function(){var a=arguments,o=P[i][1];
  return o.replace(/\$(\d)/g,function(_,n){var v=a[+n]||"";return D.hasOwnProperty(v)?D[v]:v})});
 return null}
/* a whole text, or "a, b, c" parts, or "name - message" (an upload line) */
function T(s){var k=(s||"").trim();if(!k)return s;var t=one(k);
 if(t===null&&k.indexOf(", ")>0){var ps=k.split(", "),ok=true;ps=ps.map(function(p){var x=one(p);if(x===null)ok=false;return x});if(ok)t=ps.join(", ")}
 if(t===null){var d=k.lastIndexOf(" - ");if(d>0){var x=one(k.slice(d+3));if(x!==null)t=k.slice(0,d)+" - "+x}}
 if(t===null&&/^Failed: /.test(k)){var y=one(k.slice(8));t="Falló: "+(y===null?k.slice(8):y)}
 return t===null?s:s.replace(k,t)}
function node(n){if(n.nodeType===3){var t=T(n.data);if(t!==n.data)n.data=t;return}
 if(n.nodeType!==1||n.tagName==="SCRIPT"||n.tagName==="STYLE")return;
 ["placeholder","title","aria-label"].forEach(function(a){var v=n.getAttribute(a);if(v){var t=T(v);if(t!==v)n.setAttribute(a,t)}});
 for(var c=n.firstChild;c;c=c.nextSibling)node(c)}
node(document.body);
new MutationObserver(function(ms){ms.forEach(function(m){if(m.type==="characterData")node(m.target);else m.addedNodes.forEach(node)})})
 .observe(document.body,{childList:true,subtree:true,characterData:true});
var c0=window.confirm;window.confirm=function(q){return c0.call(window,T(String(q)))};
Z.T=T;
)JS";

}  // namespace

ZF_REGISTER_WEB_MODULE([] {
  WebModule m;
  m.name = "i18n";
  m.order = 0;
  m.js = kJs;
  return m;
});

}  // namespace webtransfer
}  // namespace zf
