#include <avadb/avapager.h>

void ava_pager_init(AvaPager* pager, AvaOSInterface** interface, AvaFile** file) {
    /* If no values were specified, fill them with the default ones */
    if (pager->page_size == 0 && pager->max_page_quota == 0) {
        pager->page_size = 4096;
        pager->max_page_quota = 512;
    }
    pager->interface = interface;
    pager->file = file;
}

void ava_pager_deinit(AvaPager* pager) {
}