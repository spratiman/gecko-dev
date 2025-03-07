/* -*- Mode: indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim: set sts=2 sw=2 et tw=80: */
"use strict";

const {require} = ChromeUtils.import("resource://devtools/shared/Loader.jsm", {});
const {gDevTools} = require("devtools/client/framework/devtools");

function background() {
  browser.test.onMessage.addListener(msg => {
    let code;
    if (msg === "navigate") {
      code = "window.wrappedJSObject.location.href = 'http://example.com/';";
      browser.tabs.executeScript({code});
    } else if (msg === "reload") {
      code = "window.wrappedJSObject.location.reload(true);";
      browser.tabs.executeScript({code});
    }
  });
  browser.tabs.onUpdated.addListener((tabId, changeInfo, tab) => {
    if (changeInfo.status === "complete" && tab.url === "http://example.com/") {
      browser.test.sendMessage("tabUpdated");
    }
  });
  browser.test.sendMessage("ready");
}

function devtools_page() {
  let eventCount = 0;
  let listener = url => {
    eventCount++;
    browser.test.assertEq("http://example.com/", url, "onNavigated received the expected url.");
    browser.test.sendMessage("onNavigatedFired", eventCount);

    if (eventCount === 2) {
      eventCount = 0;
      browser.devtools.network.onNavigated.removeListener(listener);
    }
  };
  browser.devtools.network.onNavigated.addListener(listener);

  let harLogCount = 0;
  let harListener = async msg => {
    if (msg !== "getHAR") {
      return;
    }

    harLogCount++;

    const harLog = await browser.devtools.network.getHAR();
    browser.test.sendMessage("getHAR-result", harLog);

    if (harLogCount === 2) {
      harLogCount = 0;
      browser.test.onMessage.removeListener(harListener);
    }
  };
  browser.test.onMessage.addListener(harListener);

  let requestFinishedListener = async request => {
    browser.test.assertTrue(request.request, "Request entry must exist");
    browser.test.assertTrue(request.response, "Response entry must exist");

    browser.test.sendMessage("onRequestFinished");

    // Get response content using callback
    request.getContent((content, encoding) => {
      browser.test.sendMessage("onRequestFinished-callbackExecuted",
                               [content, encoding]);
    });

    // Get response content using returned promise
    request.getContent().then(([content, encoding]) => {
      browser.test.sendMessage("onRequestFinished-promiseResolved",
                               [content, encoding]);
    });

    browser.devtools.network.onRequestFinished.removeListener(requestFinishedListener);
  };
  browser.devtools.network.onRequestFinished.addListener(requestFinishedListener);
}

function waitForRequestAdded(toolbox) {
  return new Promise(resolve => {
    let netPanel = toolbox.getPanel("netmonitor");
    netPanel.panelWin.once("NetMonitor:RequestAdded", () => {
      resolve();
    });
  });
}

let extData = {
  background,
  manifest: {
    permissions: ["tabs", "http://mochi.test/", "http://example.com/"],
    devtools_page: "devtools_page.html",
  },
  files: {
    "devtools_page.html": `<!DOCTYPE html>
      <html>
        <head>
          <meta charset="utf-8">
          <script src="devtools_page.js"></script>
        </head>
        <body>
        </body>
      </html>`,
    "devtools_page.js": devtools_page,
  },
};

/**
 * Test for `chrome.devtools.network.onNavigate()` API
 */
add_task(async function test_devtools_network_on_navigated() {
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, "http://mochi.test:8888/");
  let extension = ExtensionTestUtils.loadExtension(extData);

  await extension.startup();
  await extension.awaitMessage("ready");

  let target = gDevTools.getTargetForTab(tab);

  await gDevTools.showToolbox(target, "webconsole");
  info("Developer toolbox opened.");

  extension.sendMessage("navigate");
  await extension.awaitMessage("tabUpdated");
  let eventCount = await extension.awaitMessage("onNavigatedFired");
  is(eventCount, 1, "The expected number of events were fired.");

  extension.sendMessage("reload");
  await extension.awaitMessage("tabUpdated");
  eventCount = await extension.awaitMessage("onNavigatedFired");
  is(eventCount, 2, "The expected number of events were fired.");

  // do a reload after the listener has been removed, do not expect a message to be sent
  extension.sendMessage("reload");
  await extension.awaitMessage("tabUpdated");

  await gDevTools.closeToolbox(target);

  await target.destroy();

  await extension.unload();

  await BrowserTestUtils.removeTab(tab);
});

/**
 * Test for `chrome.devtools.network.getHAR()` API
 */
add_task(async function test_devtools_network_get_har() {
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, "http://mochi.test:8888/");
  let extension = ExtensionTestUtils.loadExtension(extData);

  await extension.startup();
  await extension.awaitMessage("ready");

  let target = gDevTools.getTargetForTab(tab);

  // Open the Toolbox
  let toolbox = await gDevTools.showToolbox(target, "webconsole");
  info("Developer toolbox opened.");

  // Get HAR, it should be empty since the Net panel wasn't selected.
  const getHAREmptyPromise = extension.awaitMessage("getHAR-result");
  extension.sendMessage("getHAR");
  const getHAREmptyResult = await getHAREmptyPromise;
  is(getHAREmptyResult.log.entries.length, 0, "HAR log should be empty");

  // Select the Net panel.
  await toolbox.selectTool("netmonitor");

  // Reload the page to collect some HTTP requests.
  extension.sendMessage("navigate");

  // Wait till the navigation is complete and request
  // added into the net panel.
  await Promise.all([
    extension.awaitMessage("tabUpdated"),
    extension.awaitMessage("onNavigatedFired"),
    extension.awaitMessage("onRequestFinished"),
    extension.awaitMessage("onRequestFinished-callbackExecuted"),
    extension.awaitMessage("onRequestFinished-promiseResolved"),
    waitForRequestAdded(toolbox),
  ]);

  // Get HAR, it should not be empty now.
  const getHARPromise = extension.awaitMessage("getHAR-result");
  extension.sendMessage("getHAR");
  const getHARResult = await getHARPromise;
  is(getHARResult.log.entries.length, 1, "HAR log should not be empty");

  // Shutdown
  await gDevTools.closeToolbox(target);

  await target.destroy();

  await extension.unload();

  await BrowserTestUtils.removeTab(tab);
});

/**
 * Test for `chrome.devtools.network.onRequestFinished()` API
 */
add_task(async function test_devtools_network_on_request_finished() {
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, "http://mochi.test:8888/");
  let extension = ExtensionTestUtils.loadExtension(extData);

  await extension.startup();
  await extension.awaitMessage("ready");

  let target = gDevTools.getTargetForTab(tab);

  // Open the Toolbox
  let toolbox = await gDevTools.showToolbox(target, "netmonitor");
  info("Developer toolbox opened.");

  // Reload and wait for onRequestFinished event.
  extension.sendMessage("navigate");

  await Promise.all([
    extension.awaitMessage("tabUpdated"),
    extension.awaitMessage("onNavigatedFired"),
    waitForRequestAdded(toolbox),
  ]);

  await extension.awaitMessage("onRequestFinished");

  // Wait for response content being fetched.
  let [callbackRes, promiseRes] = await Promise.all([
    extension.awaitMessage("onRequestFinished-callbackExecuted"),
    extension.awaitMessage("onRequestFinished-promiseResolved"),
  ]);

  ok(callbackRes[0].startsWith("<html>"),
     "The expected content has been retrieved.");
  is(callbackRes[1], "text/html; charset=utf-8",
     "The expected content has been retrieved.");

  is(promiseRes[0], callbackRes[0],
     "The resolved value is equal to the one received in the callback API mode");
  is(promiseRes[1], callbackRes[1],
     "The resolved value is equal to the one received in the callback API mode");

  // Shutdown
  await gDevTools.closeToolbox(target);

  await target.destroy();

  await extension.unload();

  await BrowserTestUtils.removeTab(tab);
});
