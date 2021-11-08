// Copyright (c) 2019 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include <string>
#include <vector>

#include "include/base/cef_bind.h"
#include "include/cef_parser.h"
#include "include/cef_request_context_handler.h"
#include "include/cef_scheme.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_stream_resource_handler.h"
#include "tests/ceftests/test_handler.h"
#include "tests/ceftests/test_suite.h"
#include "tests/gtest/include/gtest/gtest.h"
#include "tests/shared/browser/client_app_browser.h"

namespace {

class SchemeHandlerFactory : public CefSchemeHandlerFactory {
 public:
  SchemeHandlerFactory(std::string data) : data_(data) {}

  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       const CefString& scheme_name,
                                       CefRefPtr<CefRequest> request) override {
    CefRefPtr<CefStreamReader> stream = CefStreamReader::CreateForData(
        static_cast<void*>(const_cast<char*>(data_.c_str())), data_.size());
    return new CefStreamResourceHandler(200, "OK", "text/html", {}, stream);
  }

 private:
  std::string data_;

  IMPLEMENT_REFCOUNTING(SchemeHandlerFactory);
};

// Browser-side app delegate.
class MediaAccessBrowserTest : public client::ClientAppBrowser::Delegate {
 public:
  MediaAccessBrowserTest() {}

  void OnBeforeCommandLineProcessing(
      CefRefPtr<client::ClientAppBrowser> app,
      CefRefPtr<CefCommandLine> command_line) override {
    // We might run tests on systems that don't have media device,
    // so just use fake devices.
    command_line->AppendSwitch("use-fake-device-for-media-stream");
  }

 private:
  IMPLEMENT_REFCOUNTING(MediaAccessBrowserTest);
};

class TestResults {
 public:
  TestResults() {}
  TrackCallback got_success;
  TrackCallback got_audio;
  TrackCallback got_video;
};

class MediaAccessTestHandler : public TestHandler {
 public:
  MediaAccessTestHandler(TestResults* tr, int32_t request, int32_t response)
      : test_results_(tr), request_(request), response_(response) {}

  cef_return_value_t OnBeforeResourceLoad(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      CefRefPtr<CefRequest> request,
      CefRefPtr<CefCallback> callback) override {
    std::string newUrl = request->GetURL();
    if (newUrl.find("tests/exit") != std::string::npos) {
      CefURLParts url_parts;
      CefParseURL(newUrl, url_parts);
      if (newUrl.find("SUCCESS") != std::string::npos) {
        test_results_->got_success.yes();
        std::string data_string = newUrl.substr(newUrl.find("&data=") +
                                                std::string("&data=").length());
        std::string data_string_decoded = CefURIDecode(
            data_string, false,
            static_cast<cef_uri_unescape_rule_t>(
                UU_SPACES | UU_URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS));
        auto obj = CefParseJSON(data_string_decoded,
                                JSON_PARSER_ALLOW_TRAILING_COMMAS);
        CefRefPtr<CefDictionaryValue> data = obj->GetDictionary();
        const auto got_video = data->GetBool("got_video_track");
        const auto got_audio = data->GetBool("got_audio_track");
        if (got_video) {
          test_results_->got_video.yes();
        }
        if (got_audio) {
          test_results_->got_audio.yes();
        }
      }
      DestroyTest();
      return RV_CANCEL;
    }

    return RV_CONTINUE;
  }

  void RunTest() override {
    std::string page =
        "<html><head>"
        "<script>"
        "function onResult(val, data) {"
        " if(!data) {"
        "   data = { got_audio_track: false, got_video_track: false};"
        " }"
        " document.location = "
        "`http://tests/"
        "exit?result=${val}&data=${encodeURIComponent(JSON.stringify(data))}`;"
        "}";

    const bool want_audio_device =
        request_ & CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE;
    const bool want_video_device =
        request_ & CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE;
    const bool want_desktop_audio =
        request_ & CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE;
    const bool want_desktop_video =
        request_ & CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE;

    if (want_audio_device || want_video_device) {
      page += std::string("navigator.mediaDevices.getUserMedia({audio: ") +
              (want_audio_device ? "true" : "false") +
              ", video: " + (want_video_device ? "true" : "false") + "})";
    } else {
      page += std::string("navigator.mediaDevices.getDisplayMedia({audio: ") +
              (want_desktop_audio ? "true" : "false") +
              ", video: " + (want_desktop_video ? "true" : "false") + "})";
    }

    page +=
        ".then(function(stream) {"
        "onResult(`SUCCESS`, {got_audio_track: stream.getAudioTracks().length "
        "> 0, got_video_track: stream.getVideoTracks().length > 0});"
        "})"
        ".catch(function(err) {"
        "console.log(err);"
        "onResult(`FAILURE`);"
        "});"
        "</script>"
        "</head><body>MEDIA ACCESS TEST</body></html>";

    // Create the request context that will use an in-memory cache.
    CefRequestContextSettings settings;
    CefRefPtr<CefRequestContext> request_context =
        CefRequestContext::CreateContext(settings, nullptr);

    // Register the scheme handler.
    request_context->RegisterSchemeHandlerFactory(
        "mcustom", "media-tests", new SchemeHandlerFactory(page));

    // Create the browser.
    CreateBrowser("mcustom://media-tests/media.html", request_context);

    // Time out the test after a reasonable period of time.
    SetTestTimeout();
  }

  void CompleteTest() {
    if (!CefCurrentlyOn(TID_UI)) {
      CefPostTask(TID_UI,
                  base::BindRepeating(&MediaAccessTestHandler::CompleteTest, this));
      return;
    }

    DestroyTest();
  }

  bool OnRequestMediaAccessPermission(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      const CefString& requesting_url,
      int32_t requested_permissions,
      CefRefPtr<CefMediaAccessCallback> callback) override {
    DCHECK(requested_permissions == request_);
    callback->Continue(response_);
    return true;
  }

 protected:
  TestResults* test_results_;
  int32_t request_;
  int32_t response_;

  IMPLEMENT_REFCOUNTING(MediaAccessTestHandler);
};
}  // namespace

// Capture device tests
TEST(MediaAccessTest, DeviceFailureWhenReturningNoPermission) {
  TestResults test_results;

  CefRefPtr<MediaAccessTestHandler> handler =
      new MediaAccessTestHandler(&test_results,
                                 CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE |
                                     CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE,
                                 CEF_MEDIA_PERMISSION_NONE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_FALSE(test_results.got_success);
  EXPECT_FALSE(test_results.got_audio);
  EXPECT_FALSE(test_results.got_video);
}

TEST(MediaAccessTest, DeviceFailureWhenRequestingAudioButReturningVideo) {
  TestResults test_results;

  CefRefPtr<MediaAccessTestHandler> handler = new MediaAccessTestHandler(
      &test_results, CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE,
      CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_FALSE(test_results.got_success);
  EXPECT_FALSE(test_results.got_audio);
  EXPECT_FALSE(test_results.got_video);
}

TEST(MediaAccessTest, DeviceFailureWhenRequestingVideoButReturningAudio) {
  TestResults test_results;

  CefRefPtr<MediaAccessTestHandler> handler = new MediaAccessTestHandler(
      &test_results, CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE,
      CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_FALSE(test_results.got_success);
  EXPECT_FALSE(test_results.got_audio);
  EXPECT_FALSE(test_results.got_video);
}

TEST(MediaAccessTest, DevicePartialFailureReturningVideo) {
  TestResults test_results;

  CefRefPtr<MediaAccessTestHandler> handler =
      new MediaAccessTestHandler(&test_results,
                                 CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE |
                                     CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE,
                                 CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_FALSE(test_results.got_success);
  EXPECT_FALSE(test_results.got_audio);
  EXPECT_FALSE(test_results.got_video);
}

TEST(MediaAccessTest, DevicePartialFailureReturningAudio) {
  TestResults test_results;

  CefRefPtr<MediaAccessTestHandler> handler =
      new MediaAccessTestHandler(&test_results,
                                 CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE |
                                     CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE,
                                 CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_FALSE(test_results.got_success);
  EXPECT_FALSE(test_results.got_audio);
  EXPECT_FALSE(test_results.got_video);
}

TEST(MediaAccessTest, DeviceFailureWhenReturningScreenCapture1) {
  TestResults test_results;

  CefRefPtr<MediaAccessTestHandler> handler =
      new MediaAccessTestHandler(&test_results,
                                 CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE |
                                     CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE,
                                 CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_FALSE(test_results.got_success);
  EXPECT_FALSE(test_results.got_audio);
  EXPECT_FALSE(test_results.got_video);
}

TEST(MediaAccessTest, DeviceFailureWhenReturningScreenCapture2) {
  TestResults test_results;

  CefRefPtr<MediaAccessTestHandler> handler =
      new MediaAccessTestHandler(&test_results,
                                 CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE |
                                     CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE,
                                 CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_FALSE(test_results.got_success);
  EXPECT_FALSE(test_results.got_audio);
  EXPECT_FALSE(test_results.got_video);
}

TEST(MediaAccessTest, DeviceFailureWhenReturningScreenCapture3) {
  TestResults test_results;

  CefRefPtr<MediaAccessTestHandler> handler = new MediaAccessTestHandler(
      &test_results, CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE,
      CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_FALSE(test_results.got_success);
  EXPECT_FALSE(test_results.got_audio);
  EXPECT_FALSE(test_results.got_video);
}

TEST(MediaAccessTest, DeviceFailureWhenReturningScreenCapture4) {
  TestResults test_results;

  CefRefPtr<MediaAccessTestHandler> handler = new MediaAccessTestHandler(
      &test_results, CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE,
      CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_FALSE(test_results.got_success);
  EXPECT_FALSE(test_results.got_audio);
  EXPECT_FALSE(test_results.got_video);
}

TEST(MediaAccessTest, DeviceFailureWhenReturningScreenCapture5) {
  TestResults test_results;

  CefRefPtr<MediaAccessTestHandler> handler = new MediaAccessTestHandler(
      &test_results, CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE,
      CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_FALSE(test_results.got_success);
  EXPECT_FALSE(test_results.got_audio);
  EXPECT_FALSE(test_results.got_video);
}

TEST(MediaAccessTest, DeviceFailureWhenReturningScreenCapture6) {
  TestResults test_results;

  CefRefPtr<MediaAccessTestHandler> handler = new MediaAccessTestHandler(
      &test_results, CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE,
      CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_FALSE(test_results.got_success);
  EXPECT_FALSE(test_results.got_audio);
  EXPECT_FALSE(test_results.got_video);
}

TEST(MediaAccessTest, DeviceSuccessAudioOnly) {
  TestResults test_results;

  CefRefPtr<MediaAccessTestHandler> handler = new MediaAccessTestHandler(
      &test_results, CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE,
      CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_TRUE(test_results.got_success);
  EXPECT_TRUE(test_results.got_audio);
  EXPECT_FALSE(test_results.got_video);
}

TEST(MediaAccessTest, DeviceSuccessVideoOnly) {
  TestResults test_results;

  CefRefPtr<MediaAccessTestHandler> handler = new MediaAccessTestHandler(
      &test_results, CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE,
      CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_TRUE(test_results.got_success);
  EXPECT_FALSE(test_results.got_audio);
  EXPECT_TRUE(test_results.got_video);
}

TEST(MediaAccessTest, DeviceSuccessAudioVideo) {
  TestResults test_results;

  CefRefPtr<MediaAccessTestHandler> handler =
      new MediaAccessTestHandler(&test_results,
                                 CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE |
                                     CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE,
                                 CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE |
                                     CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_TRUE(test_results.got_success);
  EXPECT_TRUE(test_results.got_audio);
  EXPECT_TRUE(test_results.got_video);
}

// Screen capture tests
TEST(MediaAccessTest, DesktopFailureWhenReturningNoPermission) {
  TestResults test_results;

  CefRefPtr<MediaAccessTestHandler> handler =
      new MediaAccessTestHandler(&test_results,
                                 CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE |
                                     CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE,
                                 CEF_MEDIA_PERMISSION_NONE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_FALSE(test_results.got_success);
  EXPECT_FALSE(test_results.got_audio);
  EXPECT_FALSE(test_results.got_video);
}

TEST(MediaAccessTest, DesktopFailureWhenRequestingVideoButReturningAudio) {
  TestResults test_results;

  CefRefPtr<MediaAccessTestHandler> handler = new MediaAccessTestHandler(
      &test_results, CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE,
      CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_FALSE(test_results.got_success);
  EXPECT_FALSE(test_results.got_audio);
  EXPECT_FALSE(test_results.got_video);
}

TEST(MediaAccessTest, DesktopPartialSuccessReturningVideo) {
  TestResults test_results;

  CefRefPtr<MediaAccessTestHandler> handler =
      new MediaAccessTestHandler(&test_results,
                                 CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE |
                                     CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE,
                                 CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_TRUE(test_results.got_success);
  EXPECT_FALSE(test_results.got_audio);
  EXPECT_TRUE(test_results.got_video);
}

TEST(MediaAccessTest, DesktopPartialFailureReturningAudio) {
  TestResults test_results;
  CefRefPtr<MediaAccessTestHandler> handler =
      new MediaAccessTestHandler(&test_results,
                                 CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE |
                                     CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE,
                                 CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);

  EXPECT_FALSE(test_results.got_success);
  EXPECT_FALSE(test_results.got_audio);
  EXPECT_FALSE(test_results.got_video);
}

// Entry point for registering custom schemes.
// Called from client_app_delegates.cc.
void RegisterMediaCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
  // We need a secure origin for getUserMedia to work
  // so we use a custom scheme here.
  registrar->AddCustomScheme("mcustom", CEF_SCHEME_OPTION_STANDARD |
                                            CEF_SCHEME_OPTION_SECURE |
                                            CEF_SCHEME_OPTION_CORS_ENABLED);
}

// Entry point for creating media access browser test objects.
// Called from client_app_delegates.cc.
void CreateMediaAccessBrowserTests(
    client::ClientAppBrowser::DelegateSet& delegates) {
  delegates.insert(new MediaAccessBrowserTest);
}
