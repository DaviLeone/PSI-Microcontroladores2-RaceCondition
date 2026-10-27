/*
 * DEMONSTRACAO DE RACE CONDITION (condicao de corrida) - Zephyr 4.2 / FRDM-KL25Z
 *
 * Analogia com o sistema bancario:
 *
 *   Conta com R$ 1.000,00  <->  "conta de tempo de LED" com 1000 ms de saldo
 *   Saque de R$ 800,00     <->  pedido para manter um LED aceso por 800 ms
 *   Duas requisicoes no mesmo milissegundo  <->  thread A e thread B liberadas juntas
 *
 * Cada thread executa o padrao classico "check-then-act" SEM protecao:
 *
 *      1. CHECK : le o saldo e confere se ha saldo suficiente (>= 800)
 *      2. (janela) : "consulta ao autorizador" - a thread dorme JANELA_MS
 *      3. ACT   : debita o saque (saldo = saldo - 800) e acende o LED por 800 ms
 *
 * Como as duas threads fazem o CHECK antes de qualquer uma fazer o ACT, as duas
 * enxergam saldo = 1000 e as duas "sacam". Resultado:
 *
 *      saldo final = 1000 - 800 - 800 = -600   (impossivel numa conta correta!)
 *      os DOIS LEDs acendem, consumindo 1600 ms de um saldo de 1000 ms.
 *
 * LEDs da placa:
 *      led0 -> thread A sacando
 *      led2 -> thread B sacando
 *      led1 -> ALARME: acende quando a corrida e detectada (saldo negativo)
 *
 * Leitura rapida na placa:
 *      - Apenas UM LED de saque acende e o alarme fica apagado -> comportamento correto
 *      - DOIS LEDs de saque acendem juntos e o alarme acende   -> RACE CONDITION
 *
 * Para ver a CORRECAO (mutex), compile com  -DUSAR_MUTEX=1  (ver platformio.ini)
 * ou troque o valor padrao logo abaixo.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(race_led, LOG_LEVEL_DBG);

/* 0 = SEM protecao (race condition) | 1 = protegido por mutex (correto) */
#ifndef USAR_MUTEX
#define USAR_MUTEX 0
#endif

/* ---------------------------- LEDs da placa ---------------------------- */
#define LED_A_NODE      DT_ALIAS(led0)   /* thread A */
#define LED_B_NODE      DT_ALIAS(led2)   /* thread B */
#define LED_ALARME_NODE DT_ALIAS(led1)   /* corrida detectada */

static const struct gpio_dt_spec ledA      = GPIO_DT_SPEC_GET(LED_A_NODE, gpios);
static const struct gpio_dt_spec ledB      = GPIO_DT_SPEC_GET(LED_B_NODE, gpios);
static const struct gpio_dt_spec ledAlarme = GPIO_DT_SPEC_GET(LED_ALARME_NODE, gpios);

/* ----------------------------- Parametros ------------------------------ */
#define SALDO_INICIAL      1000   /* "R$ 1.000,00" (aqui: 1000 ms de LED)      */
#define VALOR_SAQUE         800   /* "R$   800,00" (aqui:  800 ms de LED aceso) */
#define JANELA_MS           100   /* latencia entre CHECK e ACT (autorizacao)   */
#define PAUSA_RODADAS_MS   3000   /* intervalo entre uma rodada e a proxima     */

#define PRIO_THREADS          5   /* as duas threads tem a MESMA prioridade     */
#define STACK_THREAD       1024

/* --------------------------- Recurso compartilhado ---------------------- */
/*
 * Este e o "saldo da conta". Duas threads leem e escrevem nele.
 * Obs.: 'volatile' garante apenas que o compilador nao guarde o valor em
 * registrador; NAO torna a sequencia check-then-act atomica, ou seja, NAO
 * resolve a race condition.
 */
static volatile int32_t saldo = SALDO_INICIAL;

#if USAR_MUTEX
K_MUTEX_DEFINE(saldo_mutex);
#endif

/* Sincronizacao da demonstracao (nao faz parte do problema em si) */
K_SEM_DEFINE(largada, 0, 2);   /* main libera as duas threads no mesmo tick */
K_SEM_DEFINE(fim, 0, 2);       /* cada thread avisa a main que terminou      */

struct cliente {
    const char *nome;
    const struct gpio_dt_spec *led;
    volatile bool sacou;       /* true se a thread conseguiu sacar nesta rodada */
};

static struct cliente cli_A = { .nome = "A", .led = &ledA, .sacou = false };
static struct cliente cli_B = { .nome = "B", .led = &ledB, .sacou = false };

/* ------------------------------ Saque ---------------------------------- */
static void saque(struct cliente *c)
{
#if USAR_MUTEX
    k_mutex_lock(&saldo_mutex, K_FOREVER);   /* check + debito viram UMA secao critica */
#endif

    /* ---- 1. CHECK ---- */
    uint32_t t_check = k_uptime_get_32();
    int32_t visto = saldo;

    if (visto >= VALOR_SAQUE) {
        LOG_INF("[%s] t=%u ms | CHECK: saldo=%d >= %d -> saque AUTORIZADO",
                c->nome, t_check, visto, VALOR_SAQUE);

        /* ---- 2. janela de vulnerabilidade ----
         * Representa a consulta ao autorizador/banco de dados. Enquanto esta
         * thread dorme, a CPU executa a outra, que ainda ve o saldo antigo. */
        k_msleep(JANELA_MS);

        /* ---- 3. ACT: le-modifica-escreve sobre um valor que pode estar velho ---- */
        saldo = saldo - VALOR_SAQUE;
        c->sacou = true;
        LOG_INF("[%s] t=%u ms | DEBITO: saldo agora = %d",
                c->nome, k_uptime_get_32(), saldo);

#if USAR_MUTEX
        k_mutex_unlock(&saldo_mutex);
#endif

        /* Efeito fisico do saque: LED aceso durante VALOR_SAQUE ms */
        gpio_pin_set_dt(c->led, 1);
        k_msleep(VALOR_SAQUE);
        gpio_pin_set_dt(c->led, 0);
    } else {
        LOG_WRN("[%s] t=%u ms | CHECK: saldo=%d < %d -> saque NEGADO",
                c->nome, t_check, visto, VALOR_SAQUE);
#if USAR_MUTEX
        k_mutex_unlock(&saldo_mutex);
#endif
    }
}

static void thread_cliente(void *p1, void *p2, void *p3)
{
    struct cliente *c = p1;

    while (1) {
        k_sem_take(&largada, K_FOREVER);   /* espera o "tiro de largada" da main */
        saque(c);
        k_sem_give(&fim);
    }
}

K_THREAD_DEFINE(tid_A, STACK_THREAD, thread_cliente, &cli_A, NULL, NULL,
                PRIO_THREADS, 0, 0);
K_THREAD_DEFINE(tid_B, STACK_THREAD, thread_cliente, &cli_B, NULL, NULL,
                PRIO_THREADS, 0, 0);

/* ------------------------------- main ---------------------------------- */
int main(void)
{
    if (!gpio_is_ready_dt(&ledA) || !gpio_is_ready_dt(&ledB) ||
        !gpio_is_ready_dt(&ledAlarme)) {
        LOG_ERR("GPIO dos LEDs nao esta pronto");
        return 0;
    }

    gpio_pin_configure_dt(&ledA, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&ledB, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&ledAlarme, GPIO_OUTPUT_INACTIVE);

    k_thread_name_set(tid_A, "cliente_A");
    k_thread_name_set(tid_B, "cliente_B");

    LOG_INF("=== Race condition: saque simultaneo (%s) ===",
            USAR_MUTEX ? "COM mutex - correto" : "SEM protecao - com bug");
    LOG_INF("Saldo inicial=%d | cada thread tenta sacar %d | janela=%d ms",
            SALDO_INICIAL, VALOR_SAQUE, JANELA_MS);

    uint32_t rodada = 0, corridas = 0;

    /* A main tem prioridade maior que A e B (main=0 < 5): ela so executa
     * quando bloqueia em k_sem_take()/k_msleep(), entao as duas threads
     * sao liberadas dentro do mesmo tick do kernel. */
    while (1) {
        rodada++;

        /* Reinicia a conta e os LEDs */
        saldo = SALDO_INICIAL;
        cli_A.sacou = false;
        cli_B.sacou = false;
        gpio_pin_set_dt(&ledAlarme, 0);

        LOG_INF("--- Rodada %u: saldo=%d, dois saques de %d no mesmo instante ---",
                rodada, saldo, VALOR_SAQUE);

        /* Duas requisicoes "no mesmo milissegundo" */
        k_sem_give(&largada);
        k_sem_give(&largada);

        /* Espera as duas terminarem (inclui os 800 ms de LED aceso) */
        k_sem_take(&fim, K_FOREVER);
        k_sem_take(&fim, K_FOREVER);

        int32_t total_sacado = (cli_A.sacou ? VALOR_SAQUE : 0) +
                               (cli_B.sacou ? VALOR_SAQUE : 0);
        int32_t final = saldo;

        if (final < 0 || total_sacado > SALDO_INICIAL) {
            corridas++;
            gpio_pin_set_dt(&ledAlarme, 1);
            LOG_ERR("RACE CONDITION! saldo final=%d | sacado=%d de %d disponiveis",
                    final, total_sacado, SALDO_INICIAL);
        } else {
            LOG_INF("OK: saldo final=%d | sacado=%d | A=%s B=%s",
                    final, total_sacado,
                    cli_A.sacou ? "sacou" : "negado",
                    cli_B.sacou ? "sacou" : "negado");
        }
        LOG_INF("Placar: %u rodadas, %u com race condition", rodada, corridas);

        k_msleep(PAUSA_RODADAS_MS);   /* alarme fica aceso durante a pausa */
    }

    return 0;
}
