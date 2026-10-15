/* SPDX-License-Identifier: MIT */
#include "wifi_portal.h"
#include <string.h>

struct writer { char *text; size_t capacity, used; bool ok; };

static void append(struct writer *w, const char *text)
{
    const size_t length = strlen(text);
    if (!w->ok || length >= w->capacity - w->used) { w->ok = false; return; }
    memcpy(w->text + w->used, text, length);
    w->used += length;
    w->text[w->used] = 0;
}

static void escaped(struct writer *w, const char *text)
{
    for (; *text; ++text) {
        switch (*text) {
        case '&': append(w, "&amp;"); break;
        case '<': append(w, "&lt;"); break;
        case '>': append(w, "&gt;"); break;
        case '"': append(w, "&quot;"); break;
        case '\'': append(w, "&#39;"); break;
        default: {
            const unsigned char c = (unsigned char)*text;
            /* SSIDs are opaque UTF-8 strings. Refuse attribute-breaking controls
             * rather than reflecting them as literal HTML whitespace. */
            if (c < 32 || c == 127) { w->ok = false; return; }
            char one[2] = {*text, 0};
            append(w, one);
        }
        }
    }
}

size_t wifi_portal_render(char *output, size_t capacity, const char *ssid,
                          const char *csrf, bool hotspot)
{
    if (!output || !capacity) return 0;
    output[0] = 0;
    if (!ssid || !csrf || strlen(ssid) > 32 || strlen(csrf) != 32) return 0;
    for (const char *p = csrf; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
              (*p >= 'A' && *p <= 'F'))) return 0;
    struct writer w = {output, capacity, 0, true};
    append(&w, "<!doctype html><html lang=en><meta charset=utf-8>"
           "<meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>Chromatic Wi-Fi setup</title><style>"
           ":root{color-scheme:light dark;font:17px system-ui,sans-serif}"
           "body{max-width:30rem;margin:3rem auto;padding:0 1.2rem;line-height:1.5}"
           "h1{font-size:1.7rem;line-height:1.2}label{display:block;margin-top:1.1rem;font-weight:600}"
           "input,select,button{box-sizing:border-box;width:100%;font:inherit;padding:.7rem;border:1px solid #888;border-radius:.5rem}"
           "button{margin-top:1.3rem;background:#187149;color:white;border:0;font-weight:700;cursor:pointer}"
           "button:disabled{opacity:.6}small{display:block;margin-top:.35rem}"
           "#status{padding:.8rem 0;white-space:pre-line}a{color:inherit}"
           "</style><h1>Chromatic Wi-Fi setup</h1>"
           "<p>Choose how your Chromatic connects. Settings are saved on the device; no firmware update is needed.</p>"
           "<form id=setup method=post action=/wifi/save>"
           "<input type=hidden name=csrf value='");
    append(&w, csrf);
    append(&w, "'><label for=mode>Connection</label><select id=mode name=mode>"
           "<option value=station");
    if (!hotspot) append(&w, " selected");
    append(&w, ">Join an existing Wi-Fi network</option><option value=hotspot");
    if (hotspot) append(&w, " selected");
    append(&w, ">Create a Chromatic hotspot</option></select>"
           "<label for=ssid>Network name</label>"
           "<input id=ssid name=ssid required maxlength=32 autocomplete=off autocapitalize=none spellcheck=false value='");
    escaped(&w, ssid);
    append(&w, "'><label for=password>Wi-Fi password</label>"
           "<input id=password name=password type=password required minlength=8 maxlength=");
    append(&w, hotspot ? "63" : "64");
    append(&w, " autocomplete=new-password aria-describedby=hint><small id=hint>");
    append(&w, hotspot ? "Use 8–63 printable ASCII characters." : "Use 8–63 printable ASCII characters, or a 64-digit hexadecimal key.");
    append(&w, " Enter it each time you save.</small>"
           "<button id=save>Save Wi-Fi settings</button></form>"
           "<p id=status role=status aria-live=polite></p>"
           "<p>Keep this page open while the connection is tested. Close Wi-Fi setup on the Chromatic when you are finished.</p>"
           "<noscript><p>After saving, the Chromatic shows its connection status and address in the Wi-Fi menu.</p></noscript>"
           "<script>"
           "const f=document.getElementById('setup'),m=document.getElementById('mode'),"
           "p=document.getElementById('password'),h=document.getElementById('hint'),"
           "s=document.getElementById('status'),b=document.getElementById('save');"
           "let generation=0;"
           "m.onchange=()=>{const ap=m.value==='hotspot';p.maxLength=ap?63:64;"
           "h.textContent=(ap?'Use 8–63 printable ASCII characters.':'Use 8–63 printable ASCII characters, or a 64-digit hexadecimal key.')+' Enter it each time you save.';};"
           "function poll(g,n){if(g!==generation)return;"
           "fetch('/wifi/status',{cache:'no-store'}).then(r=>{if(!r.ok)throw Error();return r.json();}).then(v=>{"
           "if(g!==generation)return;"
           "if(v.state==='connected'){s.textContent='Saved. Connected to Wi-Fi. Address: '+v.ip+'\\nYou can now close Wi-Fi setup on the Chromatic.';}"
           "else if(v.state==='hotspot'){s.textContent='Saved. Your Chromatic hotspot is ready. Close Wi-Fi setup, then join the network you just saved.';}"
           "else if(v.state==='failed'){s.textContent='Settings saved, but the connection did not succeed. Check the network name and password, then try again. Wi-Fi setup remains available.';}"
           "else if(n<90)setTimeout(()=>poll(g,n+1),1000);"
           "else s.textContent='Settings saved. Check the Wi-Fi menu on the Chromatic for connection status.';"
           "}).catch(()=>{if(g!==generation)return;if(n<90)setTimeout(()=>poll(g,n+1),1000);"
           "else s.textContent='Check the Wi-Fi menu on the Chromatic for connection status. Rejoin the setup network to try again.';});}"
           "f.onsubmit=async e=>{e.preventDefault();const ap=m.value==='hotspot',pw=p.value;"
           "if((ap&&!/^[\\x20-\\x7e]{8,63}$/.test(pw))||(!ap&&!(/^[\\x20-\\x7e]{8,63}$/.test(pw)||/^[0-9a-f]{64}$/i.test(pw)))){"
           "s.textContent='Enter a valid Wi-Fi password before saving.';return;}"
           "b.disabled=true;const g=++generation;s.textContent='Saving…';"
           "try{const r=await fetch('/wifi/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','Accept':'application/json'},"
           "body:new URLSearchParams(new FormData(f))});const v=await r.json();if(!r.ok||!v.ok)throw Error();"
           "p.value='';s.textContent='Saved on the device. Testing the connection…';poll(g,0);"
           "}catch(_){s.textContent='Could not confirm the save. Check the Wi-Fi menu on the Chromatic, then reopen setup before trying again.';}"
           "finally{b.disabled=false;}};"
           "</script></html>");
    if (!w.ok) { output[0] = 0; return 0; }
    return w.used;
}

const char *wifi_portal_saved(void)
{
    return "<!doctype html><html lang=en><meta charset=utf-8>"
           "<meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>Wi-Fi settings saved</title><body style='font:18px system-ui;max-width:30rem;margin:3rem auto;padding:1rem'>"
           "<h1>Wi-Fi settings saved</h1><p>Your settings are saved on the Chromatic. "
           "Check its Wi-Fi menu for connection status and its address.</p>"
           "<p>Close Wi-Fi setup on the Chromatic when you are finished.</p>"
           "<p><a href=/wifi>Return to Wi-Fi setup</a></p></body></html>";
}
