// main/todo_model.c
#include "todo_model.h"

#include <stddef.h>

void todo_model_init(todo_model_t *model, const todo_item_t *items,
                     uint8_t *states, int count)
{
    model->items = items;
    model->states = states;
    model->count = count;
    for (int i = 0; i < count; i++) {
        states[i] = (uint8_t)items[i].state;
    }
}

int todo_model_total(const todo_model_t *model)
{
    return model->count;
}

int todo_model_done_count(const todo_model_t *model)
{
    int done = 0;
    for (int i = 0; i < model->count; i++) {
        if (model->states[i] == TODO_STATE_DONE) done++;
    }
    return done;
}

int todo_model_page_count(const todo_model_t *model)
{
    if (model->count <= 0) return 1;
    return (model->count + TODO_PAGE_SIZE - 1) / TODO_PAGE_SIZE;
}

int todo_model_visible_on_page(const todo_model_t *model, int page)
{
    if (page < 0) return 0;
    int first = page * TODO_PAGE_SIZE;
    if (first >= model->count) return 0;
    int rest = model->count - first;
    return rest > TODO_PAGE_SIZE ? TODO_PAGE_SIZE : rest;
}

int todo_model_global_index(int page, int row)
{
    if (page < 0 || row < 0) return -1;
    int index = page * TODO_PAGE_SIZE + row;
    if (row >= TODO_PAGE_SIZE) return -1;
    return index;
}

todo_state_t todo_model_state(const todo_model_t *model, int global_index)
{
    if (global_index < 0 || global_index >= model->count) return TODO_STATE_PENDING;
    return (todo_state_t)model->states[global_index];
}

const todo_item_t *todo_model_item(const todo_model_t *model, int global_index)
{
    if (global_index < 0 || global_index >= model->count) return NULL;
    return &model->items[global_index];
}

todo_state_t todo_model_toggle(todo_model_t *model, int global_index)
{
    if (global_index < 0 || global_index >= model->count) return TODO_STATE_PENDING;

    todo_state_t cur = (todo_state_t)model->states[global_index];
    todo_state_t next;
    switch (cur) {
    case TODO_STATE_PENDING:
    case TODO_STATE_URGENT:
        next = TODO_STATE_DONE;
        break;
    case TODO_STATE_DONE:
    default:
        next = TODO_STATE_PENDING;
        break;
    }
    model->states[global_index] = (uint8_t)next;
    return next;
}

