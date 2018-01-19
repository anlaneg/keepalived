/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        List structure manipulation.
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

#include "list.h"
#include "memory.h"

/* Simple list helpers functions */
list
alloc_list(void (*free_func) (void *), void (*dump_func) (void *))
{
	return alloc_mlist(free_func, dump_func, 1);
}

static element
alloc_element(void)
{
	element new = (element) MALLOC(sizeof (struct _element));
	return new;
}

//申请element并将其加入到list
void
list_add(list l, void *data)
{
	element e = alloc_element();

	e->prev = l->tail;
	/* e->next = NULL;	// MALLOC sets this NULL */
	e->data = data;

	if (l->head == NULL)
		l->head = e;
	else
		l->tail->next = e;
	l->tail = e;
	l->count++;
}

//找到data,并将其自list中删除
void
list_del(list l, void *data)
{
	element e;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		if (ELEMENT_DATA(e) == data) {
			if (e->prev)
				e->prev->next = e->next;
			else
				l->head = e->next;

			if (e->next)
				e->next->prev = e->prev;
			else
				l->tail = e->prev;

			l->count--;
			FREE(e);
			return;
		}
	}
}

//返回list中第num个元素的data
void *
list_element(list l, size_t num)
{
	element e = LIST_HEAD(l);
	size_t i = 0;

	/* fetch element number num */
	for (i = 0; i < num; i++) {
		if (!e)
			return NULL;

		ELEMENT_NEXT(e);
	}

	if (e)
		return ELEMENT_DATA(e);
	return NULL;
}

//遍历list，并调用dump
void
dump_list(list l)
{
	element e;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e))
		if (l->dump)
			(*l->dump) (e->data);
}

//遍历list,并针对每一个list元素，调用list上的free函数
static void
free_elements(list l)
{
	element e;
	element next;

	for (e = LIST_HEAD(l); e; e = next) {
		next = e->next;
		if (l->free)
			(*l->free) (e->data);
		l->count--;
		FREE(e);
	}
#if 0
	if (l->count)
		log_message(LOG_INFO, "free_elements left %d elements on the list", l->count);
#endif
}

//销毁list
void
free_list_elements(list l)
{
	free_elements(l);

	l->head = NULL;
	l->tail = NULL;
}

//销毁list，并置list为空
void
free_list(list *lp)
{
	list l = *lp;

	if (!l)
		return;

	/* Remove the caller's reference to the list */
	*lp = NULL;

	free_elements(l);
	FREE(l);
}

//删除l中的元素e,并释放其data占用的内存
void
free_list_element(list l, element e)
{
	if (!l || !e)
		return;
	if (l->head == e)
		l->head = e->next;
	else
		e->prev->next = e->next;
	if (l->tail == e)
		l->tail = e->prev;
	else
		e->next->prev = e->prev;
	if (l->free)
		(*l->free) (e->data);
	l->count--;
	FREE(e);
}

/* Multiple list helpers functions */
//申请list,设置list对应的回调，容许同时创建多个list
list
alloc_mlist(void (*free_func) (void *), void (*dump_func) (void *), size_t size)
{
	list new = (list) MALLOC(size * sizeof (struct _list));
	new->free = free_func;
	new->dump = dump_func;
	return new;
}

#ifdef _INCLUDE_UNUSED_CODE_
void
dump_mlist(list l, size_t size)
{
	element e;
	int i;

	for (i = 0; i < size; i++) {
		for (e = LIST_HEAD(&l[i]); e; ELEMENT_NEXT(e))
			if (l->dump)
				(*l->dump) (e->data);
	}
}
#endif

//针对每个list的element，调用free_func函数
static void
free_melement(list l, void (*free_func) (void *))
{
	element e;
	element next;

	for (e = LIST_HEAD(l); e; e = next) {
		next = e->next;
		if (free_func)
			(*free_func) (e->data);
		FREE(e);
	}
}

//一次性释放掉多个list(有size个）
void
free_mlist(list l, size_t size)
{
	size_t i;

	if (!l)
		return;

	for (i = 0; i < size; i++)
		free_melement(&l[i], l->free);
	FREE(l);
}
