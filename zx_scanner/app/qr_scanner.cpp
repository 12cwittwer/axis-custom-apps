/**
 * Copyright (C) 2021 Axis Communications AB, Lund, Sweden
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
#include <opencv2/imgproc.hpp>
#pragma GCC diagnostic pop
#include <opencv2/video.hpp>
#include <stdlib.h>
#include <syslog.h>
#include <opencv2/imgcodecs.hpp>
#include <curl/curl.h>
#include <axsdk/axparameter.h>
#include <axsdk/axevent.h>
#include <glib-object.h>
#include <glib.h>

#include <ZXing/ReadBarcode.h>
#include "send_event.h"
#include "imgprovider.h"

#define APP_NAME "parkspass_qr_scanner"

using namespace cv;

static AXEventHandler* event_handler = nullptr;
static guint qr_event_id = 0;
static ImgProvider_t* provider = nullptr;
static Mat bgr_mat;
static Mat nv12_mat;
static Mat grey_mat;
static cv::Rect roi;
static std::string endpoint;
static std::string auth;
static std::string location;
static std::string device_id;
static gboolean delay_in_progress = FALSE;

static bool uploadRecentEntries(const std::string& json_data, const std::string& endpoint, const std::string& auth, const std::string location, const std::string device_id);
static bool retrieveAxParameters(std::string& endpoint, std::string& auth, std::string& location, std::string& device_id);
static gboolean process_frame(AppData* app_data);
static gboolean reset_delay_flag(gpointer user_data);

int main(void) {
    GMainLoop* main_loop = NULL;

    // Open app logs
    openlog(APP_NAME, LOG_PID | LOG_CONS, LOG_USER);
    syslog(LOG_INFO, "Running %s example with VDO as video source", APP_NAME);

    // Retrieve the AxParameters from manifest file
    if (!retrieveAxParameters(endpoint, auth, location, device_id)) {
        return EXIT_FAILURE;
    }

    // The desired width and height of the BGR frame
    unsigned int width  = 1280;
    unsigned int height = 720;

    // chooseStreamResolution gets the least resource intensive stream
    // that exceeds or equals the desired resolution specified above
    unsigned int streamWidth  = 0;
    unsigned int streamHeight = 0;
    if (!chooseStreamResolution(width, height, &streamWidth, &streamHeight)) {
        syslog(LOG_ERR, "%s: Failed choosing stream resolution", __func__);
        exit(1);
    }

    syslog(LOG_INFO,
           "Creating VDO image provider and creating stream %d x %d",
           streamWidth,
           streamHeight);
    provider = createImgProvider(streamWidth, streamHeight, 2, VDO_FORMAT_YUV);
    if (!provider) {
        syslog(LOG_ERR, "%s: Failed to create ImgProvider", __func__);
        exit(2);
    }

    syslog(LOG_INFO, "Start fetching video frames from VDO");
    if (!startFrameFetch(provider)) {
        syslog(LOG_ERR, "%s: Failed to fetch frames from VDO", __func__);
        exit(3);
    }

    // Create OpenCV Mats for the camera frame (nv12), the converted frame (bgr)
    // and the foreground frame that is outputted by the background subtractor
    bgr_mat  = Mat(streamHeight, streamWidth, CV_8UC3);
    nv12_mat = Mat(streamHeight  * 3 / 2, streamWidth, CV_8UC1);

    // Crop area being scanned
    roi = cv::Rect(streamWidth * 1 / 3, streamHeight * 1 / 4, streamWidth * 1 / 3, streamHeight * 1 / 2);

    // Set up event
    AppData* app_data = create_event();
    syslog(LOG_INFO, "New event created with ID: %d", app_data->event_id);

    g_timeout_add(100, (GSourceFunc)process_frame, app_data);
    main_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(main_loop);

    event_cleanup();
    destroyImgProvider(provider);

    g_main_loop_unref(main_loop);
    return EXIT_SUCCESS;
}

static gboolean process_frame(AppData* app_data) {
    if (delay_in_progress) {
        return TRUE;
    }
    // Get the latest NV12 image frame from VDO using the imageprovider
    VdoBuffer* buf = getLastFrameBlocking(provider);
    if (!buf) {
        syslog(LOG_INFO, "No more frames available, exiting");
        return FALSE;
    }

    nv12_mat.data = static_cast<uint8_t*>(vdo_buffer_get_data(buf));

    // imwrite("nv12.png", nv12_mat);
    // syslog(LOG_INFO, "nv12 image saved to nv12.png");

    // Convert the NV12 data to BGR
    cvtColor(nv12_mat, bgr_mat, COLOR_YUV2BGR_NV12, 3);

    // imwrite("bgr_frame.png", bgr_mat);
    // syslog(LOG_INFO, "BGR image saved to bgr_frame.png");

    cvtColor(bgr_mat, grey_mat, COLOR_BGR2GRAY);

    // imwrite("greyscale_img.png", grey_mat);
    // syslog(LOG_INFO, "Greyscale image saved to greyscale_img.png");

    cv::Mat cropped = grey_mat(roi);

    // Resize the cropped image to enhance resolution
    cv::Mat resized;
    cv::resize(cropped, resized, cv::Size(), 2.0, 2.0, cv::INTER_CUBIC); // 2x enlargement

    cv::Mat result = resized.clone();

    // Increase black and white contrast
    cv::Mat high_contrast;
    resized.convertTo(high_contrast, -1, 2.5, -200); // Increase contrast

    result = high_contrast; // Inserted to allow removal of thresholding. Needs to be cleaned up later.

    // Set all pixels with a brightness greater than 100 to white
    // Thresholding does not work properly in low light environments
    // double threshold_value = 100;
    // for (int y = 0; y < high_contrast.rows; ++y) {
    //     for (int x = 0; x < high_contrast.cols; ++x) {
    //         if (high_contrast.at<uchar>(y, x) >= threshold_value) {
    //             result.at<uchar>(y, x) = 255; // Set pixel to white
    //         } else if (high_contrast.at<uchar>(y,x) < threshold_value) {
    //             result.at<uchar>(y,x) = 0; // Set pixel to black
    //         }
    //     }
    // }

    imwrite("altered_img.png", result);
    // syslog(LOG_INFO, "Final photo saved to altered_img.png");

    auto image = ZXing::ImageView(result.data, result.cols, result.rows, ZXing::ImageFormat::Lum);
    auto options = ZXing::ReaderOptions().setFormats(ZXing::BarcodeFormat::QRCode);
    auto barcodes = ZXing::ReadBarcodes(image, options);

    for (const auto& b : barcodes) {
        syslog(LOG_INFO, "%s: %s", ZXing::ToString(b.format()).c_str(), b.text().c_str());
        // Uncomment when QR scanner is working effectively
        if(uploadRecentEntries(b.text(), endpoint, auth, location, device_id)) {
            app_data->value = 1;
            send_event(app_data);

            delay_in_progress = TRUE;
            g_timeout_add(3000, reset_delay_flag, NULL);
        } else {
            app_data->value = 2;
            send_event(app_data);

            delay_in_progress = TRUE;
            g_timeout_add(3000, reset_delay_flag, NULL);
        }
    }

    returnFrame(provider, buf);
    return TRUE;
}


static bool uploadRecentEntries(const std::string& json_data, const std::string& endpoint, const std::string& auth, const std::string location, const std::string device_id) {

    std::string json_body = "{"
        "\"location\": \"" + location + "\","
        "\"device_id\": \"" + device_id + "\","
        "\"data\": \"" + json_data +
    "\"}";
    
    CURL* curl;
    CURLcode res;
    char error_buffer[CURL_ERROR_SIZE];

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if (curl) {
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        std::string auth_header = "PARKSPLUS_AUTH: " + auth;
        headers = curl_slist_append(headers, auth_header.c_str());

        curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);  // Follow redirects
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);       // Limit the number of redirects to follow

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            syslog(LOG_INFO, "curl_easy_perform() failed");
        } else {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            if (http_code == 200) {
                syslog(LOG_INFO, "Data uploaded succesfully");
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                curl_global_cleanup();
                return true;
            } else {
                syslog(LOG_INFO, "Data was not successfully uploaded");
            }
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    } else {
        syslog(LOG_ERR, "Failed to initialize CURL");
    }

    curl_global_cleanup();
    return false;
}

static bool retrieveAxParameters(std::string& endpoint, std::string& auth, std::string& location, std::string& device_id) {
    GError* error = nullptr;

    // Create AXParameter handle
    AXParameter* handle = ax_parameter_new(APP_NAME, &error);
    if (!handle) {
        syslog(LOG_ERR, "Failed to create AXParameter: %s", error->message);
        if (error) g_error_free(error); // Free error object
        return false;
    }

    // Cleanup handle on exit
    auto cleanup = [&]() { ax_parameter_free(handle); };
    try {
        gchar *param_value = NULL;

        // Retrieve parameters
        if (ax_parameter_get(handle, "ENDPOINT", &param_value, &error)) {
            endpoint = param_value;
            g_free(param_value);
        } else {
            syslog(LOG_ERR, "Failed to retrieve ENDPOINT");
        }
        if (ax_parameter_get(handle, "AUTH", &param_value, &error)) {
            auth = param_value;
            g_free(param_value);
        } else {
            syslog(LOG_ERR, "Failed to retrieve AUTH");
        }
        if (ax_parameter_get(handle, "LOCATION", &param_value, &error)) {
            location = param_value;
            g_free(param_value);
        } else {
            syslog(LOG_ERR, "Failed to retrieve LOCATION");
        }
        if (ax_parameter_get(handle, "DEVICE", &param_value, &error)) {
            device_id = param_value;
            g_free(param_value);
        } else {
            syslog(LOG_ERR, "Failed to retrieve DEVICE");
        }

        // Log parameters for debugging
        syslog(LOG_INFO, "Endpoint: %s", endpoint.c_str());
        syslog(LOG_INFO, "Auth: %s", auth.c_str());
        syslog(LOG_INFO, "Location: %s", location.c_str());
        syslog(LOG_INFO, "Device ID: %s", device_id.c_str());

        if (error) g_error_free(error); // Free error object
    } catch (const std::exception& ex) {
        syslog(LOG_ERR, "%s", ex.what());
        cleanup();
        if (error) g_error_free(error); // Free error object
        return false;
    }

    // Cleanup resources
    return true;
}

static gboolean reset_delay_flag(gpointer user_data) {
    delay_in_progress = FALSE;
    return FALSE;
}
