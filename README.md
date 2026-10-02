# Relatório - Atividade de Programação Concorrente

**Problema:** Sistema de Logging Assíncrono com buffer de mensagens
**Linguagem:** C (threads POSIX - `pthread_mutex_t` e `pthread_cond_t`)
**Configuração:** 4 threads produtoras, 1 thread consumidora, buffer de 10 posições e 20 mensagens de log por produtor (80 mensagens no total).

---

## 1. Código-fonte completo

O código completo está no arquivo [`sistema-logging.c`](./sistema-logging.c). Ele está organizado em quatro partes:

- **Definições:** constantes (`THREAD_NUM`, `CAPACIDADE`, `ITENSPROD`, `ATRASO_PROD_US`, `ATRASO_CONS_US`), a struct `logMessage` (a mensagem de log) e a struct `bufferMonitor` (o monitor).
- **Operações do monitor:** `monitor_init`, `monitor_destroy`, `pushMessage`, `popMessage` e `closeMonitor`.
- **Threads:** a função `produtor` (executada por cada thread produtora) e a função `consumidor` (thread de escrita do log).
- **`main`:** cria o monitor e as threads, aguarda o término de todas com `pthread_join` e destrói o monitor.

---

## 2. Instruções para compilação e execução

**Requisitos:** compilador GCC e sistema com suporte a threads POSIX (Linux, macOS ou WSL).

Comando para compilação:

```bash
gcc -Wall -Wextra -pthread sistema-logging.c -o sistema-logging
```

Comando para execução:

```bash
./sistema-logging
```

A flag `-pthread` é necessária para ligar a biblioteca de threads. As flags `-Wall -Wextra` apenas habilitam avisos adicionais do compilador.

Por causa dos atrasos simulados com `usleep`, a execução leva cerca de 2 a 3 segundos.

*(Opcional)* Para verificar a ausência de condições de corrida com o ThreadSanitizer:

```bash
gcc -fsanitize=thread -g -pthread sistema-logging.c -o sistema-logging-tsan
./sistema-logging-tsan
```

---

## 3. Explicação da arquitetura do monitor

O programa implementa o padrão **produtor-consumidor com buffer circular limitado**. Como a linguagem C não possui monitores nativos, o monitor foi construído por convenção: a struct `bufferMonitor` reúne **os dados compartilhados**, **um mutex** e **duas variáveis de condição**, e esses dados só são acessados pelas funções do monitor, que sempre travam o mutex na entrada e o liberam na saída.

Componentes do monitor:

| Componente | Descrição |
|---|---|
| `buffer[CAPACIDADE]` | Vetor circular que armazena as mensagens de log ainda não consumidas. |
| `front`, `rear`, `ocupacao` | Controlam as posições de leitura e escrita e a quantidade de mensagens no buffer. |
| `fechado`, `prodsAtivos` | Controlam o encerramento: quantos produtores ainda estão ativos e se a produção terminou. |
| `mtx` | Mutex que garante a exclusão mútua em todas as operações do monitor. |
| `buffer_naocheio`, `buffer_naovazio` | Variáveis de condição usadas para a coordenação entre produtores e consumidor. |

**Funcionamento geral:** cada produtor gera suas mensagens localmente (nível aleatório, texto correspondente) e as insere com `pushMessage`. O consumidor retira as mensagens com `popMessage` e as imprime. Para tornar o comportamento visível, o produtor "dorme" um tempo aleatório entre duas mensagens (simulando a chegada de eventos) e o consumidor "dorme" após imprimir cada log (simulando o custo de gravação); esses atrasos não participam da sincronização. Quando um produtor termina suas 20 mensagens, ele chama `closeMonitor`. Quando o último produtor termina, o monitor passa a estado "fechado", e o consumidor encerra depois de esvaziar o buffer.

---

## 4. Identificação de todas as variáveis compartilhadas

Todas as variáveis compartilhadas estão dentro da única instância global `monitor` (do tipo `bufferMonitor`) e só são acessadas pelas operações do monitor, sempre com `mtx` travado.

| Variável | Tipo | Função | Protegida por |
|---|---|---|---|
| `buffer[CAPACIDADE]` | `logMessage[10]` | Armazena as mensagens pendentes | `mtx` |
| `front` | `int` | Índice da próxima leitura | `mtx` |
| `rear` | `int` | Índice da próxima escrita | `mtx` |
| `ocupacao` | `int` | Número de mensagens atualmente no buffer | `mtx` |
| `fechado` | `int` | Indica que todos os produtores terminaram | `mtx` |
| `prodsAtivos` | `int` | Número de produtores que ainda não terminaram | `mtx` |

**Variáveis que não são compartilhadas** (locais a cada thread, portanto não precisam de proteção): `id`, `seed` e `msg` em cada produtor, e `mensagem` no consumidor. A mensagem é passada **por valor** para `pushMessage` e copiada para uma variável local em `popMessage`, de modo que a impressão no consumidor ocorre fora do monitor.

Observação: ao final, a thread consumidora lê `monitor.ocupacao` sem travar o mutex para imprimir a ocupação final. Isso é seguro porque, nesse ponto, o monitor está fechado e vazio e nenhuma outra thread o acessa mais.

---

## 5. Descrição das operações públicas do monitor

| Operação | Chamada por | Descrição | Retorno |
|---|---|---|---|
| `monitor_init(m)` | `main` (antes de criar as threads) | Inicializa os campos, o mutex e as variáveis de condição. `prodsAtivos` começa com `THREAD_NUM`. | `void` |
| `pushMessage(m, log)` | Produtores | Insere uma mensagem no buffer. Bloqueia enquanto o buffer estiver cheio. | `void` |
| `popMessage(m, log)` | Consumidor | Retira a mensagem mais antiga do buffer e a copia para `*log`. Bloqueia enquanto o buffer estiver vazio e a produção não tiver sido encerrada. | `1` se retirou uma mensagem; `0` se o buffer está vazio e a produção terminou |
| `closeMonitor(m)` | Produtores (um por produtor, ao terminar) | Registra que um produtor terminou. Quando o último termina, marca o monitor como fechado e acorda as threads em espera. | `void` |
| `monitor_destroy(m)` | `main` (depois de todos os `join`) | Destrói o mutex e as variáveis de condição. | `void` |

`monitor_init` e `monitor_destroy` são chamadas apenas antes da criação e depois do término das threads, portanto nunca concorrem com as demais operações.

---

## 6. Identificação das condições que fazem cada operação esperar

| Operação | Condição de espera | Variável de condição |
|---|---|---|
| `pushMessage` | `ocupacao == CAPACIDADE` (buffer cheio) | `buffer_naocheio` |
| `popMessage` | `ocupacao == 0 && !fechado` (buffer vazio e ainda podem chegar mensagens) | `buffer_naovazio` |
| `closeMonitor` | Nenhuma (apenas aguarda a aquisição do mutex) | - |

Se `popMessage` encontra `ocupacao == 0` **e** `fechado`, ela **não** espera: libera o mutex e retorna `0`, indicando ao consumidor que não há mais mensagens.

Todas as esperas estão dentro de um laço `while`, de modo que a condição é **reavaliada sempre que a thread acorda**:

```c
while (m->ocupacao == CAPACIDADE)
    pthread_cond_wait(&m->buffer_naocheio, &m->mtx);
```

---

## 7. Explicação das variáveis de condição utilizadas

O programa usa duas variáveis de condição, cada uma associada a uma condição lógica distinta:

| Variável | Condição que representa | Quem espera | Quem sinaliza |
|---|---|---|---|
| `buffer_naocheio` | "Há espaço livre no buffer" | `pushMessage` | `popMessage` (`signal`) e `closeMonitor` (`broadcast`) |
| `buffer_naovazio` | "Há mensagem para consumir, ou a produção foi encerrada" | `popMessage` | `pushMessage` (`signal`) e `closeMonitor` (`broadcast`) |

**Como funciona `pthread_cond_wait(&cond, &mtx)`:** de forma atômica, libera o mutex e bloqueia a thread na variável de condição, sem consumir CPU (não há espera ocupada). Ao ser acordada, a thread readquire o mutex antes de retornar. A liberação do mutex é o que permite que outra thread entre no monitor e altere o estado.

**Por que o `while` (e não `if`):** o `pthread_cond_wait` pode retornar sem que ninguém tenha sinalizado (*spurious wakeup*), e, entre o `signal` e o momento em que a thread acordada readquire o mutex, outra thread pode ter alterado o estado do buffer. Por isso a condição é sempre testada novamente, o que garante o funcionamento correto independentemente da ordem de escalonamento.

**Por que `broadcast` no `closeMonitor`:** o encerramento é um evento que interessa a *todas* as threads em espera, e não a apenas uma. Com `broadcast`, o programa continua correto mesmo que se aumente o número de consumidores. O `broadcast` em `buffer_naocheio` é uma medida defensiva: com o projeto atual, quando `prodsAtivos` chega a zero nenhum produtor está mais em `pushMessage`.

---

## 8. Explicação de onde ocorre a sinalização de outras threads

Todas as sinalizações são feitas **com o mutex travado**, antes do `pthread_mutex_unlock`.

| Local | Sinalização | Quem é acordado | Motivo |
|---|---|---|---|
| `pushMessage`, após inserir a mensagem | `pthread_cond_signal(&buffer_naovazio)` | Consumidor em espera | O buffer deixou de estar vazio |
| `popMessage`, após retirar a mensagem | `pthread_cond_signal(&buffer_naocheio)` | Um produtor em espera | Abriu-se uma vaga no buffer |
| `closeMonitor`, quando `prodsAtivos == 0` | `pthread_cond_broadcast(&buffer_naovazio)` | Todos os consumidores em espera | A produção terminou: o consumidor precisa reavaliar a condição e encerrar |
| `closeMonitor`, quando `prodsAtivos == 0` | `pthread_cond_broadcast(&buffer_naocheio)` | Todos os produtores em espera (se houver) | Medida defensiva (ver seção 7) |

---

## 9. Identificação das regiões críticas protegidas pelo monitor

O corpo de cada operação, entre `pthread_mutex_lock` e `pthread_mutex_unlock`, é uma região crítica:

- **`pushMessage`:** teste de `ocupacao`, escrita em `buffer[rear]`, atualização de `rear` e incremento de `ocupacao`.
- **`popMessage`:** teste de `ocupacao` e `fechado`, leitura de `buffer[front]`, atualização de `front` e decremento de `ocupacao`.
- **`closeMonitor`:** decremento de `prodsAtivos` e atribuição de `fechado`.

**Trechos que ficam fora do monitor** (por operarem apenas em dados locais): a geração da mensagem pelo produtor (sorteio do nível e cópia do texto), a impressão da mensagem pelo consumidor com `printf` e as duas chamadas a `usleep`. Assim, operações lentas (como a escrita na saída ou a simulação de processamento) não mantêm o mutex travado e não bloqueiam as demais threads.

---

## 10. Descrição da estratégia de encerramento das threads

O encerramento é feito pela operação `close` do monitor, sem contagem fixa de mensagens no consumidor e sem mensagens sentinela:

1. Cada produtor, ao inserir suas 20 mensagens, chama `closeMonitor`, que decrementa `prodsAtivos`.
2. O **último** produtor a terminar (`prodsAtivos == 0`) marca `fechado = 1` e acorda as threads em espera com `broadcast`. Como os atrasos aleatórios fazem os produtores terminarem em momentos diferentes, os primeiros apenas decrementam o contador e o monitor continua aberto.
3. O consumidor continua retirando mensagens enquanto houver alguma no buffer. A condição de parada é `ocupacao == 0 && fechado`: `popMessage` retorna `0` e o laço do consumidor termina. Assim, **nenhuma mensagem é perdida**, mesmo que o monitor feche com o buffer ainda cheio.
4. O consumidor imprime a mensagem final de encerramento e retorna.
5. A `main` aguarda **todas as threads** com `pthread_join` (4 produtores e 1 consumidor) e então chama `monitor_destroy`.

A memória alocada com `malloc` para passar o identificador a cada produtor é liberada pelo próprio produtor (`free`) logo no início da sua execução.

---

## 11. Exemplo de execução do programa

Execução:

```bash
./sistema-logging
```

Trecho da saída (primeiras linhas, um trecho do meio e as últimas linhas). A ordem das mensagens varia a cada execução, pois depende do escalonamento das threads e dos atrasos aleatórios:

```text
A thread 0 produziu um log de level 2 com a seguinte mensagem: Info: informações que destacam o progresso normal da aplicação.
A thread 1 produziu um log de level 4 com a seguinte mensagem: Error: falhas que impedem o funcionamento de uma funcionalidade específica
A thread 2 produziu um log de level 2 com a seguinte mensagem: Info: informações que destacam o progresso normal da aplicação.
A thread 3 produziu um log de level 2 com a seguinte mensagem: Info: informações que destacam o progresso normal da aplicação.
[...]
A thread 1 produziu um log de level 4 com a seguinte mensagem: Error: falhas que impedem o funcionamento de uma funcionalidade específica
A thread 2 produziu um log de level 0 com a seguinte mensagem: Trace: informações granulares do código e muito verbosas.
A thread 0 produziu um log de level 4 com a seguinte mensagem: Error: falhas que impedem o funcionamento de uma funcionalidade específica
A thread 3 produziu um log de level 2 com a seguinte mensagem: Info: informações que destacam o progresso normal da aplicação.
A thread 1 produziu um log de level 1 com a seguinte mensagem: Debug: informações para investigação e solução de problemas.
A thread 2 produziu um log de level 0 com a seguinte mensagem: Trace: informações granulares do código e muito verbosas.
[...]
A thread 3 produziu um log de level 4 com a seguinte mensagem: Error: falhas que impedem o funcionamento de uma funcionalidade específica
A thread 3 produziu um log de level 1 com a seguinte mensagem: Debug: informações para investigação e solução de problemas.
Produção de logs encerrada. Ocupação atual do buffer: 0
```

**Verificação:** a saída completa tem 81 linhas (80 mensagens de log, 20 de cada produtor, mais a linha final de encerramento) e a execução leva cerca de 2,4 segundos. Nas execuções testadas, todas as mensagens foram consumidas, o programa terminou sem travar e o ThreadSanitizer não reportou nenhuma condição de corrida. Para conferir a quantidade de linhas:

```bash
./sistema-logging | wc -l    # deve imprimir 81
```

**O que os atrasos mostram:** com os valores atuais (`ATRASO_PROD_US = 100000` e `ATRASO_CONS_US = 30000`), os quatro produtores geram mensagens, em média, mais rápido do que o consumidor consegue gravá-las. O buffer de 10 posições enche, e os produtores passam a bloquear em `pushMessage`, esperando em `buffer_naocheio` até que o consumidor abra uma vaga. Em testes com um contador temporário no laço de espera, isso ocorreu dezenas de vezes por execução.

**Experimentando outros valores:** os atrasos são constantes no início do código e podem ser alterados para observar o outro lado da coordenação. Reduzindo `ATRASO_CONS_US` para um valor pequeno (por exemplo, `5000`), o consumidor passa a ser mais rápido que os produtores, o buffer fica vazio com frequência e é o consumidor que bloqueia em `popMessage`, esperando em `buffer_naovazio`. Em ambos os casos o programa termina corretamente com as 80 mensagens consumidas.