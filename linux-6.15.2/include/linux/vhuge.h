/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VHUGE_H
#define _LINUX_VHUGE_H

#include <linux/gfp_types.h>
#include <linux/types.h>

struct folio;
struct mm_struct;
struct page;

enum vhuge_hp_source {
	VHUGE_HP_SRC_THP_FAULT,
	VHUGE_HP_SRC_KHUGEPAGED,
	VHUGE_HP_SRC_DAMON_PROBE,
	VHUGE_HP_SRC_MIGRATED,
};

#if IS_ENABLED(CONFIG_VHUGE_KM)
struct folio *vhuge_alloc_real_folio(struct mm_struct *mm, unsigned long addr,
				     gfp_t gfp, unsigned int order, int nid);
void vhuge_prepare_allocated_page(struct page *page, unsigned int order, gfp_t gfp);
bool vhuge_prepare_free_page(struct page *page, unsigned int order);
void vhuge_free_prepared_page(struct page *page, unsigned int order);
bool vhuge_capture_prepared_page(struct page *page, unsigned int order);
void vhuge_note_allocated_folio(struct folio *folio,
				enum vhuge_hp_source source,
				struct mm_struct *mm);
void vhuge_forget_folio(struct folio *folio);
#else
static inline struct folio *vhuge_alloc_real_folio(struct mm_struct *mm,
						   unsigned long addr, gfp_t gfp,
						   unsigned int order, int nid)
{
	return NULL;
}

static inline void vhuge_prepare_allocated_page(struct page *page,
						unsigned int order, gfp_t gfp)
{
}

static inline bool vhuge_prepare_free_page(struct page *page,
					   unsigned int order)
{
	return false;
}

static inline void vhuge_free_prepared_page(struct page *page,
					    unsigned int order)
{
}

static inline bool vhuge_capture_prepared_page(struct page *page,
					       unsigned int order)
{
	return false;
}

static inline void vhuge_note_allocated_folio(struct folio *folio,
					      enum vhuge_hp_source source,
					      struct mm_struct *mm)
{
}

static inline void vhuge_forget_folio(struct folio *folio)
{
}
#endif

#endif /* _LINUX_VHUGE_H */
