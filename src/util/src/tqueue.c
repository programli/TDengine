/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "os.h"
#include "tlog.h"
#include "taoserror.h"
#include "tqueue.h"

typedef struct _taos_qnode {
  struct _taos_qnode *next;
  char                item[];
} STaosQnode;

typedef struct _taos_q {
  int32_t             itemSize;
  int32_t             numOfItems;
  struct _taos_qnode *head;
  struct _taos_qnode *tail;
  struct _taos_q     *next;    // for queue set
  struct _taos_qset  *qset;    // for queue set
  pthread_mutex_t     mutex;  
} STaosQueue;

typedef struct _taos_qset {
  STaosQueue        *head;
  STaosQueue        *current;
  pthread_mutex_t    mutex;
  int32_t            numOfQueues;
  int32_t            numOfItems;
} STaosQset;

typedef struct _taos_qall {
  STaosQnode   *current;
  STaosQnode   *start;
  int32_t       itemSize;
  int32_t       numOfItems;
} STaosQall; 
  
taos_queue taosOpenQueue(int itemSize) {
  
  STaosQueue *queue = (STaosQueue *) calloc(sizeof(STaosQueue), 1);
  if (queue == NULL) {
    terrno = TSDB_CODE_NO_RESOURCE;
    return NULL;
  }

  pthread_mutex_init(&queue->mutex, NULL);
  queue->itemSize = (int32_t)itemSize;

  return queue;
}

void taosCloseQueue(taos_queue param) {
  STaosQueue *queue = (STaosQueue *)param;
  STaosQnode *pTemp;
  STaosQnode *pNode = queue->head;  
  queue->head = NULL;

  pthread_mutex_lock(&queue->mutex);

  if (queue->qset) taosRemoveFromQset(queue->qset, queue); 

  while (pNode) {
    pTemp = pNode;
    pNode = pNode->next;
    free (pTemp);
  }

  pthread_mutex_unlock(&queue->mutex);

  free(queue);
}

int taosWriteQitem(taos_queue param, void *item) {
  STaosQueue *queue = (STaosQueue *)param;

  STaosQnode *pNode = (STaosQnode *)calloc(sizeof(STaosQnode) + queue->itemSize, 1);
  if ( pNode == NULL ) {
    terrno = TSDB_CODE_NO_RESOURCE;
    return -1;
  }

  memcpy(pNode->item, item, queue->itemSize);

  pthread_mutex_lock(&queue->mutex);

  if (queue->tail) {
    queue->tail->next = pNode;
    queue->tail = pNode;
  } else {
    queue->head = pNode;
    queue->tail = pNode; 
  }

  queue->numOfItems++;
  if (queue->qset) atomic_add_fetch_32(&queue->qset->numOfItems, 1);

  pthread_mutex_unlock(&queue->mutex);

  return 0;
}

int taosReadQitem(taos_queue param, void *item) {
  STaosQueue *queue = (STaosQueue *)param;
  STaosQnode *pNode = NULL;
  int         code = 0;

  pthread_mutex_lock(&queue->mutex);

  if (queue->head) {
      pNode = queue->head;
      memcpy(item, pNode->item, queue->itemSize);
      queue->head = pNode->next;
      if (queue->head == NULL) 
        queue->tail = NULL;
      free(pNode);
      queue->numOfItems--;
      if (queue->qset) atomic_sub_fetch_32(&queue->qset->numOfItems, 1);
      code = 1;
  } 

  pthread_mutex_unlock(&queue->mutex);

  return code;
}

int taosReadAllQitems(taos_queue param, taos_qall *res) {
  STaosQueue *queue = (STaosQueue *)param;
  STaosQall  *qall = NULL;
  int         code = 0;

  pthread_mutex_lock(&queue->mutex);

  if (queue->head) {
    qall = (STaosQall *) calloc(sizeof(STaosQall), 1);
    if ( qall == NULL ) {
      terrno = TSDB_CODE_NO_RESOURCE;
      code = -1;
    } else {
      qall->current = queue->head;
      qall->start = queue->head;
      qall->numOfItems = queue->numOfItems;
      qall->itemSize = queue->itemSize;
      code = qall->numOfItems;

      queue->head = NULL;
      queue->tail = NULL;
      queue->numOfItems = 0;
      if (queue->qset) atomic_sub_fetch_32(&queue->qset->numOfItems, qall->numOfItems);
    }
  } 

  pthread_mutex_unlock(&queue->mutex);
  
  *res = qall;
  return code; 
}

int taosGetQitem(taos_qall param, void *item) {
  STaosQall  *qall = (STaosQall *)param;
  STaosQnode *pNode;
  int         num = 0;

  pNode = qall->current;
  if (pNode)
    qall->current = pNode->next;
 
  if (pNode) {
    memcpy(item, pNode->item, qall->itemSize);
    num = 1;
  }

  return num;
}

void taosResetQitems(taos_qall param) {
  STaosQall  *qall = (STaosQall *)param;
  qall->current = qall->start;
}

void taosFreeQitems(taos_qall param) {
  STaosQall  *qall = (STaosQall *)param;
  STaosQnode *pNode;

  while (qall->current) {
    pNode = qall->current;
    qall->current = pNode->next;
    free(pNode);
  }

  free(qall);
}

taos_qset taosOpenQset() {

  STaosQset *qset = (STaosQset *) calloc(sizeof(STaosQset), 1);
  if (qset == NULL) {
    terrno = TSDB_CODE_NO_RESOURCE;
    return NULL;
  }

  pthread_mutex_init(&qset->mutex, NULL);

  return qset;
}

void taosCloseQset(taos_qset param) {
  STaosQset *qset = (STaosQset *)param;
  free(qset);
}

int taosAddIntoQset(taos_qset p1, taos_queue p2) {
  STaosQueue *queue = (STaosQueue *)p2;
  STaosQset  *qset = (STaosQset *)p1;

  if (queue->qset) return -1; 

  pthread_mutex_lock(&qset->mutex);

  queue->next = qset->head;
  qset->head = queue;
  qset->numOfQueues++;

  pthread_mutex_lock(&queue->mutex);
  atomic_add_fetch_32(&qset->numOfItems, queue->numOfItems);
  queue->qset = qset;
  pthread_mutex_unlock(&queue->mutex);

  pthread_mutex_unlock(&qset->mutex);

  return 0;
}

void taosRemoveFromQset(taos_qset p1, taos_queue p2) {
  STaosQueue *queue = (STaosQueue *)p2;
  STaosQset  *qset = (STaosQset *)p1;
 
  STaosQueue *tqueue;

  pthread_mutex_lock(&qset->mutex);

  if (qset->head) {
    if (qset->head == queue) {
      qset->head = qset->head->next;
      qset->numOfQueues--;
    } else {
      STaosQueue *prev = qset->head;
      tqueue = qset->head->next;
      while (tqueue) {
        if (tqueue== queue) {
          prev->next = tqueue->next;
          if (qset->current == queue) qset->current = tqueue->next;
          qset->numOfQueues--;

          pthread_mutex_lock(&queue->mutex);
          atomic_sub_fetch_32(&qset->numOfItems, queue->numOfItems);
          queue->qset = NULL;
          pthread_mutex_unlock(&queue->mutex);
        } else {
          prev = tqueue;
          tqueue = tqueue->next;
        }
      }
    }
  } 
  
  pthread_mutex_unlock(&qset->mutex);
}

int taosGetQueueNumber(taos_qset param) {
  return ((STaosQset *)param)->numOfQueues;
}

int taosReadQitemFromQset(taos_qset param, void *item) {
  STaosQset  *qset = (STaosQset *)param;
  STaosQnode *pNode = NULL;
  int         code = 0;

  for(int i=0; i<qset->numOfQueues; ++i) {
    pthread_mutex_lock(&qset->mutex);
    if (qset->current == NULL) 
      qset->current = qset->head;   
    STaosQueue *queue = qset->current;
    if (queue) qset->current = queue->next;
    pthread_mutex_unlock(&qset->mutex);
    if (queue == NULL) break;

    pthread_mutex_lock(&queue->mutex);

    if (queue->head) {
        pNode = queue->head;
        memcpy(item, pNode->item, queue->itemSize);
        queue->head = pNode->next;
        if (queue->head == NULL) 
          queue->tail = NULL;
        free(pNode);
        queue->numOfItems--;
        atomic_sub_fetch_32(&qset->numOfItems, 1);
        code = 1;
    } 

    pthread_mutex_unlock(&queue->mutex);
    if (pNode) break;
  }

  return code; 
}

int taosReadAllQitemsFromQset(taos_qset param, taos_qall *res) {
  STaosQset  *qset = (STaosQset *)param;
  STaosQueue *queue;
  STaosQall  *qall = NULL;
  int         code = 0;

  for(int i=0; i<qset->numOfQueues; ++i) {
    pthread_mutex_lock(&qset->mutex);
    if (qset->current == NULL) 
      qset->current = qset->head;   
    queue = qset->current;
    if (queue) qset->current = queue->next;
    pthread_mutex_unlock(&qset->mutex);
    if (queue == NULL) break;

    pthread_mutex_lock(&queue->mutex);

    if (queue->head) {
      qall = (STaosQall *) calloc(sizeof(STaosQall), 1);
      if (qall == NULL) {
        terrno = TSDB_CODE_NO_RESOURCE;
        code = -1;
      } else {
        qall->current = queue->head;
        qall->start = queue->head;
        qall->numOfItems = queue->numOfItems;
        qall->itemSize = queue->itemSize;
        code = qall->numOfItems;
          
        queue->head = NULL;
        queue->tail = NULL;
        queue->numOfItems = 0;
        atomic_sub_fetch_32(&qset->numOfItems, qall->numOfItems);
      }
    } 

    pthread_mutex_unlock(&queue->mutex);

    if (code != 0) break;  
  }

  *res = qall;

  return code;
}

int taosGetQueueItemsNumber(taos_queue param) {
  STaosQueue *queue = (STaosQueue *)param;
  return queue->numOfItems;
}

int taosGetQsetItemsNumber(taos_qset param) {
  STaosQset *qset = (STaosQset *)param;
  return qset->numOfItems;
}
