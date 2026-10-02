#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h> // usleep

#define THREAD_NUM 4 // qtd de produtores
#define CAPACIDADE 10 // tamanho do buffer
#define ITENSPROD 20 // mensagens por produtor
#define ATRASO_PROD_US 100000 // intervalo máximo (us) entre mensagens de um produtor: simula a chegada de eventos
#define ATRASO_CONS_US 30000 // tempo (us) para "gravar" cada log: simula processamento

typedef struct{
    int thread_id;
    int level;
    char message[256];
} logMessage;

typedef struct{ // Definindo o monitor
    logMessage buffer[CAPACIDADE];
    int front;
    int rear;
    int ocupacao;
    int fechado;
    int prodsAtivos;

    pthread_mutex_t mtx;
    pthread_cond_t buffer_naocheio;
    pthread_cond_t buffer_naovazio;
} bufferMonitor;

bufferMonitor monitor;

void monitor_init(bufferMonitor *m){
    m->front = 0;
    m->rear = 0;
    m->ocupacao = 0;
    m->fechado = 0;
    m->prodsAtivos = THREAD_NUM;
    pthread_mutex_init(&m->mtx, NULL);
    pthread_cond_init(&m->buffer_naocheio, NULL);
    pthread_cond_init(&m->buffer_naovazio, NULL);
}

void monitor_destroy(bufferMonitor *m){
    pthread_mutex_destroy(&m->mtx);
    pthread_cond_destroy(&m->buffer_naocheio);
    pthread_cond_destroy(&m->buffer_naovazio);
}

void pushMessage(bufferMonitor *m, logMessage log){
    pthread_mutex_lock(&m->mtx); // Acesso ao monitor (região compartilhada)
    while(m->ocupacao == CAPACIDADE)
    {
        // Buffer cheio, agora a thread espera até que haja espaço
        pthread_cond_wait(&m->buffer_naocheio, &m->mtx);
    }
    m->buffer[m->rear] = log;
    m->rear = (m->rear + 1) % CAPACIDADE;
    m->ocupacao++;

    pthread_cond_signal(&m->buffer_naovazio);
    pthread_mutex_unlock(&m->mtx);
}

void closeMonitor(bufferMonitor *m){
    pthread_mutex_lock(&m->mtx);
    m->prodsAtivos--;
    if (m->prodsAtivos == 0){
        m->fechado = 1;
        pthread_cond_broadcast(&m->buffer_naovazio); // acorda todos os consumidores
        pthread_cond_broadcast(&m->buffer_naocheio); // acorda todos os produtores presos na operação push, pois o buffer fechou
    }
    pthread_mutex_unlock(&m->mtx);
}

int popMessage(bufferMonitor *m, logMessage *log){
    pthread_mutex_lock(&m->mtx); // Acesso ao monitor (região compartilhada)
    while(m->ocupacao == 0 && !m->fechado)
    {
        //Buffer vazio, o consumidor deve esperar até que não esteja mais vazio
        pthread_cond_wait(&m->buffer_naovazio, &m->mtx);
    }
    if (m->ocupacao == 0 && m->fechado){ // buffer vazio e produção encerrada
        pthread_mutex_unlock(&m->mtx);
        return 0;
    }

    *log = m->buffer[m->front];
    m->front = (m->front + 1) % CAPACIDADE;
    m->ocupacao--;

    pthread_cond_signal(&m->buffer_naocheio);
    pthread_mutex_unlock(&m->mtx);
    return 1;
}


void *produtor(void* arg){
    int id = *(int*)arg;
    free(arg);
    unsigned int seed = time(NULL) ^ (id * 7919); //uma seed por thread
    for (int i = 0; i < ITENSPROD; i++)
    {
        logMessage msg;
        msg.thread_id = id;
        msg.level = rand_r(&seed) % 6;
        if (msg.level == 0) strcpy(msg.message, "Trace: informações granulares do código e muito verbosas.");
        else if (msg.level == 1) strcpy(msg.message, "Debug: informações para investigação e solução de problemas.");
        else if (msg.level == 2) strcpy(msg.message, "Info: informações que destacam o progresso normal da aplicação.");
        else if (msg.level == 3) strcpy(msg.message, "Warn: situações que podem ser danosas, mas não necessariamente impedem o funcionamento.");
        else if (msg.level == 4) strcpy(msg.message, "Error: falhas que impedem o funcionamento de uma funcionalidade específica");
        else if (msg.level == 5) strcpy(msg.message, "Fatal: erros críticos que causam parada total do sistema");

        pushMessage(&monitor, msg);
        usleep(rand_r(&seed) % ATRASO_PROD_US); // simula o intervalo até o próximo evento (fora do monitor)
    }
    closeMonitor(&monitor);
    return NULL;
}

void *consumidor(void *arg){
    (void)arg;
    logMessage mensagem;

    // Condição de parada do consumidor. O loop while só termina se o buffer estiver vazio e a produção for encerrada, conforme a operação close
    while (popMessage(&monitor, &mensagem))
    {
        printf("A thread %d produziu um log de level %d com a seguinte mensagem: %s\n", mensagem.thread_id, mensagem.level, mensagem.message);
        usleep(ATRASO_CONS_US); // simula o custo de gravar o log (fora do monitor)
    }
    printf("Produção de logs encerrada. Ocupação atual do buffer: %d\n", monitor.ocupacao);
    return NULL;
}

int main()
{
    pthread_t produtoras[THREAD_NUM];
    pthread_t consumidora;
    monitor_init(&monitor);

    for (int i = 0; i < THREAD_NUM; i++)
    {
        int* a = malloc(sizeof(int));
        *a = i;
        pthread_create(&produtoras[i], NULL, produtor, a);
    }
    pthread_create(&consumidora, NULL, consumidor, NULL);

    for (int i = 0; i < THREAD_NUM; i++)
    {
        pthread_join(produtoras[i], NULL);
    }
    pthread_join(consumidora, NULL);

    monitor_destroy(&monitor);
    return 0;
}