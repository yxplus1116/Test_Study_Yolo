#include <config.h>

static const char* labels[] = {
    "body", "head"
};

void options::init()
{
    cout << "选择截图方式：0、1" << endl;
    cin >> capture;
    if (capture)
        dxgi.init();
    else
        dc.init();

    // 初始化识别功能为开启状态
    is_recognition_active = true;
}

void options::main_function()
{
    auto engine = Yolo::create_infer(
        model_file,                // engine file
        Yolo::Type::V5,                       // yolo type, Yolo::Type::V5 / Yolo::Type::X
        0,                   // gpu id
        0.5f,                      // confidence threshold
        0.45f,                      // nms threshold
        Yolo::NMSMethod::FastGPU,   // NMS method, fast GPU / CPU
        1024,                       // max objects
        true                       // preprocess use multi stream
    );

    // 在这里定义 box 和上一次空格键按下的状态变量
    ObjectDetector::BoxArray box;
    bool prev_space_state = false; // 上一次空格键按下的状态，默认为未按下

    while (1)
    {
        auto start = std::chrono::system_clock::now();
        cv::Mat frame;
        if (capture)
            frame = dxgi.get_img(do_not_show_windows);
        else
            frame = dc.CaptureScreen(do_not_show_windows);
        if (frame.empty())
        {
            continue;
        }

        if (is_recognition_active) {
            box = engine->commit(frame).get(); // 删除此行的auto关键字
            if ((!box.empty()) && KEY_DOWN(VK_LBUTTON) || KEY_DOWN(VK_RBUTTON))
            {
                mouse.fire(frame, box);
            }
            else
            {
                mouse.is_first_frame = true;
                mouse.lost_frame = 0;
                mouse.pid.refresh();
            }
        }

        auto end = std::chrono::system_clock::now();
        //cout << "FPS: " << 1000 / std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << endl;
        if (KEY_DOWN(VK_UP) && mouse.isHead == 0) {
            mouse.isHead = 1;
            std::cout << "成功切换！当前瞄准位置：<头部>" << std::endl;
        }
        else if (KEY_DOWN(VK_DOWN) && mouse.isHead == 1) {
            mouse.isHead = 0;
            std::cout << "成功切换！当前瞄准位置：<身体>" << std::endl;
        }

        if (KEY_DOWN(VK_LEFT) && !left_key_pressed) {
            // 方向键左键关闭识别功能，并记录方向键左键已被按下
            is_recognition_active = false;
            left_key_pressed = true;
            std::cout << "识别功能已关闭" << std::endl;
        }
        else if (!KEY_DOWN(VK_LEFT)) {
            // 如果方向键左键没有被按下，则将 left_key_pressed 设置为 false，以便下次按下方向键左键可以再次关闭识别功能
            left_key_pressed = false;
        }

        if (KEY_DOWN(VK_RIGHT) && !right_key_pressed) {
            // 方向键右键开启识别功能，并记录方向键右键已被按下
            is_recognition_active = true;
            right_key_pressed = true;
            std::cout << "识别功能已开启" << std::endl;
        }
        else if (!KEY_DOWN(VK_RIGHT)) {
            // 如果方向键右键没有被按下，则将 right_key_pressed 设置为 false，以便下次按下方向键右键可以再次开启识别功能
            right_key_pressed = false;
        }

        if (is_show_windows)
        {
            draw_objects(frame, box, mouse.isHead);
            putText(frame, "fps:" + std::to_string(1000 / std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count()), Point(10, 50), FONT_HERSHEY_PLAIN, 1.6, Scalar(0, 0, 255), 2);
            imshow("识别窗口", frame);
            HWND hWnd = (HWND)cvGetWindowHandle("img");
            HWND hRawWnd = ::GetParent(hWnd);

            if (NULL != hRawWnd)
            {
                BOOL bRet = ::SetWindowPos(hRawWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
                assert(bRet);
            }
            waitKey(1);
        }

        if (KEY_DOWN(VK_HOME) && mouse.is_use_hardware == 0)
        {
            dxgi.release();
            dxgi.init();
        }
    }
    engine.reset();
    dxgi.release();
}

void options::draw_objects(Mat img, ObjectDetector::BoxArray box, int is_head)
{
    for (auto& obj : box) {
        if (obj.class_label != is_head)
            continue;
        cv::rectangle(img, cv::Point(obj.left, obj.top), cv::Point(obj.right, obj.bottom), cv::Scalar(0, 255, 255));

        auto name = labels[obj.class_label];
        auto caption = iLogger::format("%s %.2f", name, obj.confidence);
        cv::putText(img, caption, cv::Point(obj.left, obj.top - 5), 0, 1, cv::Scalar(0, 255, 255));
    }
}
