#include <cli/cli.h>
#include <lib/toolbox/args.h>

#include <lib/subghz/subghz_worker.h>
#include <lib/subghz/receiver.h>
#include <lib/subghz/registry.h>
#include <lib/subghz/protocols/protocol_items.h>

//#include "protocols/test.h"

#include <gui/gui.h>

#define SUBGHZ_FREQUENCY_RANGE_STR "299999755...348000000 or 386999938...464000000 or 778999847...928000000"
#define TAG "Radio"

bool running = true;

typedef enum {
    RadioEventTypeTick,
    RadioEventTypeInput,
} RadioEventType;

typedef struct {
    RadioEventType type;
    InputEvent input;
} RadioEvent;

//struct MySubGhzKeystore {
//    SubGhzKeyArray_t data;
//};

const SubGhzProtocol* radio_protocol_registry_items[] = {
    &subghz_protocol_keeloq,
    &subghz_protocol_star_line,
    &subghz_protocol_scher_khan,
//    &protocol_test,
};

const SubGhzProtocolRegistry radio_protocol_registry = {
    .items = radio_protocol_registry_items,
    .size = COUNT_OF(radio_protocol_registry_items)
};

static void radio_draw_callback(Canvas* canvas, void* ctx) {
    UNUSED(ctx);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Radio");
}

static void radio_input_callback(InputEvent* input_event, void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* event_queue = ctx;

    RadioEvent event = {.type = RadioEventTypeInput, .input = *input_event};
    furi_message_queue_put(event_queue, &event, FuriWaitForever);
}

void radio_cli_command_print_usage() {
    printf("Usage:\r\n");
    printf("radio <cmd> <args>\r\n");
    printf("Cmd list:\r\n");
    printf("\trx <frequency in Hz>\t - Receive signal\r\n");
}

static void radio_cli_command_rx_callback(SubGhzReceiver* receiver, SubGhzProtocolDecoderBase* decoder_base, void* context) {
    char buf[100];
    size_t buf_cnt;

    Cli* cli = context;

    FuriString* text = furi_string_alloc();
    subghz_protocol_decoder_base_get_string(decoder_base, text);
    subghz_receiver_reset(receiver);
//    printf("%s", furi_string_get_cstr(text));
    buf_cnt = snprintf(buf, sizeof(buf), "%s", furi_string_get_cstr(text));
    cli_write(cli, (uint8_t*) buf, buf_cnt);
    furi_string_free(text);
}

void radio_cli_command_rx(Cli* cli, FuriString* args, void* context) {
    UNUSED(context);

    uint32_t frequency = 433920000;

    if (furi_string_size(args)) {
        int ret = sscanf(furi_string_get_cstr(args), "%lu", &frequency);
        if (ret != 1) {
            printf("sscanf returned %d, frequency: %lu\r\n", ret, frequency);
            cli_print_usage("radio rx", "<Frequency in Hz>", furi_string_get_cstr(args));
            return;
        }
        if (!furi_hal_subghz_is_frequency_valid(frequency)) {
            printf("Frequency must be in " SUBGHZ_FREQUENCY_RANGE_STR " range, not %lu\r\n", frequency);
            return;
        }
    }

    SubGhzEnvironment* environment = subghz_environment_alloc();
    subghz_environment_load_keystore(environment, EXT_PATH("subghz/assets/keeloq_mfcodes"));
    subghz_environment_load_keystore(environment, EXT_PATH("subghz/assets/keeloq_mfcodes_user"));
//    subghz_environment_set_came_atomo_rainbow_table_file_name(environment, EXT_PATH("subghz/assets/came_atomo"));
//    subghz_environment_set_nice_flor_s_rainbow_table_file_name(environment, EXT_PATH("subghz/assets/nice_flor_s"));
    subghz_environment_set_protocol_registry(environment, (void*) &radio_protocol_registry);

//    SubGhzKeystore* keystore = subghz_environment_get_keystore(environment);
//    SubGhzKeyArray_t* keys = subghz_keystore_get_data(keystore);
//    SubGhzKeyArray_t* keys = &((struct MySubGhzKeystore*)(keystore))->data;
//    for M_EACH(code, *keys, SubGhzKeyArray_t) {
//        printf("%d %llX %s\r\n", code->type, code->key, furi_string_get_cstr(code->name));
//    }

    SubGhzWorker* worker = subghz_worker_alloc();
    subghz_worker_set_overrun_callback(worker, (SubGhzWorkerOverrunCallback) subghz_receiver_reset);
    subghz_worker_set_pair_callback(worker, (SubGhzWorkerPairCallback) subghz_receiver_decode);

    SubGhzReceiver* receiver = subghz_receiver_alloc_init(environment);
    subghz_receiver_set_filter(receiver, SubGhzProtocolFlag_Decodable);
    subghz_worker_set_context(worker, receiver);
    subghz_receiver_set_rx_callback(receiver, radio_cli_command_rx_callback, cli);

    // Configure radio
    furi_hal_subghz_reset();
    furi_hal_subghz_load_preset(FuriHalSubGhzPresetOok650Async);
    frequency = furi_hal_subghz_set_frequency_and_path(frequency);
    furi_hal_gpio_init(&gpio_cc1101_g0, GpioModeInput, GpioPullNo, GpioSpeedLow);

    furi_hal_power_suppress_charge_enter();

    furi_hal_subghz_flush_rx();
    furi_hal_subghz_rx();

    // Prepare and start RX
    furi_hal_subghz_start_async_rx(subghz_worker_rx_callback, worker);

    subghz_worker_start(worker);

    // Wait for packets to arrive
    running = true;
    printf("Listening at %lu. Press CTRL+C to stop\r\n", frequency);
    while (running && !cli_cmd_interrupt_received(cli)) {
        furi_delay_ms(250);
    }
    printf("Stopping\r\n");
    running = false;

    // Shutdown radio
    furi_hal_subghz_stop_async_rx();
    furi_hal_subghz_sleep();

    furi_hal_power_suppress_charge_exit();

    if (subghz_worker_is_running(worker))
        subghz_worker_stop(worker);
    subghz_worker_free(worker);

    // Cleanup
    subghz_receiver_free(receiver);
    subghz_environment_free(environment);
}

void radio_cli_command(Cli* cli, FuriString* args, void* context) {
    FuriString* cmd;
    cmd = furi_string_alloc();

    do {
        if (!args_read_string_and_trim(args, cmd)) {
            radio_cli_command_print_usage();
            break;
        }

        if (furi_string_cmp_str(cmd, "rx") == 0) {
            radio_cli_command_rx(cli, args, context);
            break;
        }

        radio_cli_command_print_usage();
    } while (false);

    furi_string_free(cmd);
}

int32_t radio_app(void* p) {
    UNUSED(p);

    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(RadioEvent));

    // Configure view port
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, radio_draw_callback, NULL);
    view_port_input_callback_set(view_port, radio_input_callback, event_queue);

    // Register view port in GUI
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    Cli* cli = furi_record_open(RECORD_CLI);
    cli_add_command(cli, "radio", CliCommandFlagParallelSafe, radio_cli_command, NULL);

    RadioEvent event;

    running = true;
    while (1) {
        furi_check(furi_message_queue_get(event_queue, &event, FuriWaitForever) == FuriStatusOk);
        if (event.type == RadioEventTypeInput) {
            if ((event.input.type == InputTypeShort) && (event.input.key == InputKeyBack)) {
                break;
            }
        }
    }
    running = false;
    furi_delay_ms(250);

    cli_delete_command(cli, "radio");
    furi_record_close(RECORD_CLI);

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);

    furi_record_close(RECORD_GUI);

    return 0;
}
