#include <stdlib.h>
#include <avadb/avapager.h>

void ava_pager_init(AvaPager* pager, AvaOSInterface** interface, AvaFile** file) {
    /* If no values were specified, fill them with the default ones */
    if (pager->page_size == 0 && pager->max_page_quota == 0) {
        pager->page_size = 4096;
        pager->max_page_quota = 512;
    }
    pager->interface = interface;
    pager->file = file;
    pager->page_nums = calloc(pager->max_page_quota,sizeof(ava_pgid_t));
    pager->pages = calloc(pager->max_page_quota,pager->page_size);
}

void ava_pager_deinit(AvaPager* pager) {
    /* Free the pager's page cache */
    if (pager->page_nums != NULL)
        free(pager->page_nums);
    if (pager->pages != NULL)
        free(pager->pages);
}