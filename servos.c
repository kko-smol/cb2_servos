#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>  //заголовочный файл работы с прерываниями
#include <asm/io.h>
#include <linux/delay.h> //функции задержек
#include <linux/fs.h>
#include <linux/cdev.h>
#include <asm/uaccess.h>
#include <linux/pci.h>
#include <linux/slab.h>

struct servo {
   int mask; // mask for gpio
   int period;  //period for timer
   struct servo* next;  //pointer to next servo
   struct servo* prev;  //
   char name[10];
   struct class_attribute servo_class; //pointer to sys/class struct
};

struct shadow_servo{
   int mask; // mask for gpio
   int period;  //period for timer
   struct shadow_servo* next;  //pointer to next servo
};

#define SERVO_COUNT 28
static struct shadow_servo* (*__shadow_servos); //for interrups, copy from __servos
static struct servo* (*_servos);  //for user

static void* memory;

#define IRQ 99 //tmr4 interrupt
static int irq = IRQ;

static void* __gpio_map;
static void* __PD;

static void* __tmr_base;
static void* __tmr_flags;
static void* __tmr_per;

static int __shadow_D;

static struct shadow_servo* __current_shadow_servo;
static struct shadow_servo* __shadow_pause;
static bool __have_updates;

void update_shadow(unsigned long d){
    //MUTEX for SYS_STORE
    struct servo* current_servo=0;
    int i=0;
    while(current_servo!=_servos[SERVO_COUNT]->next){
        if (i==0){
            current_servo=_servos[SERVO_COUNT]->next;
            __shadow_servos[i]->period=current_servo->period;
            __shadow_servos[i]->mask=_servos[SERVO_COUNT]->mask;
            i++;
        } else {
            if (__shadow_servos[i-1]->period!=0){
                __shadow_servos[i]->mask=__shadow_servos[i-1]->mask&(~current_servo->prev->mask);
                __shadow_servos[i]->period=current_servo->period-current_servo->prev->period;
                __shadow_servos[i-1]->next=__shadow_servos[i];
                i++;
            } else {
                __shadow_servos[i-1]->mask&=(~current_servo->prev->mask);
                __shadow_servos[i-1]->period=current_servo->period-current_servo->prev->period;
            }
        }
        current_servo=current_servo->next;
    }
    __shadow_servos[i-1]->next=__shadow_servos[0]; //set this as next for prev
    __shadow_pause=__shadow_servos[i];
    //clear mutex
}

DECLARE_TASKLET( __update_shadow_tasklet , update_shadow, 0 );

static int __find_servo_by_class(struct class_attribute *attr){
    int i;
    for (i=0;i<SERVO_COUNT;i++){
        if ((&(attr->attr.name))==(&(_servos[i]->servo_class.attr.name))){return i;}
    }
    return -1;
}

//этой функцией можно будет прочитать текущее значение
static ssize_t servo_show( struct class *class, struct class_attribute *attr, char *buf ){
    //strcpy( buf, __buf_msg );
    int i;
    for (i=0;i<=SERVO_COUNT;i++){
        printk("Servo %d, name %s, mask %d, period %d, next: %s, prev: %s \n",i,_servos[i]->name,_servos[i]->mask,_servos[i]->period,_servos[i]->next->name,_servos[i]->prev->name);
    }
    printk("Shadow copy:\n");
    struct shadow_servo* current_servo = __shadow_servos[SERVO_COUNT]->next;
    i=0;
    while(i<=SERVO_COUNT){
        printk("Slot %d, mask %d, period %d, next: %d\n",i,__shadow_servos[i]->mask,__shadow_servos[i]->period,__shadow_servos[i]->next);
        i++;
    }
    return strlen( buf );
}

static ssize_t servo_store( struct class *class, struct class_attribute *attr, const char *buf, size_t count ) {
    int input;
    int servo_num;
    char __buf_msg[10];
    struct servo* temp_pos;
    __have_updates=false;
    tasklet_disable(&__update_shadow_tasklet);
    servo_num=__find_servo_by_class(attr);
    if ((count<=8)&&(servo_num>=0)){
      strncpy( &__buf_msg, buf, count ); //получили строку из пространства пользователя в модуль
      __buf_msg[count] = '\0'; //добавим символ перевода строки. необходим для kstrtoint
      if (!kstrtoint(&__buf_msg,10,&input)){ //преобразуем в число. Если 0 - преобразовалось
        input=input*240; //коэффициент
        if ((input>=0)&&(input<=48000)){ //проверяем диапазон
           //обновляем значение импульса
           _servos[servo_num]->period=input+12000;
           temp_pos=_servos[SERVO_COUNT];
           //удаляемся из списка
           if ((_servos[servo_num]->next!=NULL)&&(_servos[servo_num]->prev!=NULL)){
               //соединили соседей
               _servos[servo_num]->next->prev=_servos[servo_num]->prev;
               _servos[servo_num]->prev->next=_servos[servo_num]->next;
               //удалили свои ссылки
               _servos[servo_num]->next=NULL;
               _servos[servo_num]->prev=NULL;
           }
           //и перемещаем серву в списке в правильное положение
           while(temp_pos->prev->period>_servos[servo_num]->period){
               temp_pos=temp_pos->prev;
               if (temp_pos==_servos[SERVO_COUNT]){
                   temp_pos=temp_pos->next;
                   break;
               }
           }
           _servos[servo_num]->next=temp_pos;
           _servos[servo_num]->prev=temp_pos->prev;
           temp_pos->prev->next=_servos[servo_num];
           temp_pos->prev=_servos[servo_num];
           _servos[SERVO_COUNT]->mask|=1<<servo_num;
        }
        if (input==-240){
            _servos[servo_num]->prev->next=_servos[servo_num]->next;
            _servos[servo_num]->next->prev=_servos[servo_num]->prev;
            _servos[servo_num]->next=0;
            _servos[servo_num]->prev=0;
        }
     if (ioread32(__tmr_base+0x50)==0){
        printk("Starting timer\n");
        update_shadow(0);
        __current_shadow_servo=__shadow_servos[0];
        iowrite32(0x87,__tmr_base+0x50); //запускаем таймер: 24МГц, без делител$
     }
 

     } else {
        printk( "Servo value not updated: not number\n");
    }
    } else {
        if (servo_num==-1){
            //stop servo
        } else {
            printk( "Servo value not updated: too big\n");
        }
    }
    //shedule tasklet for update
    tasklet_hi_schedule(&__update_shadow_tasklet);
    __have_updates=true;
    return count;
}

//указатель на структуру в /sys/class
static struct class *servo_class;
CLASS_ATTR( servo, 0666, &servo_show, &servo_store);

// обработчик прерывания
static irqreturn_t my_interrupt( int irq, void *dev_id ) {
    __raw_writel(0x1<<4,__tmr_flags);  //сброс флага прерывания
    __raw_writel(__current_shadow_servo->mask,__PD);
    __raw_writel(__current_shadow_servo->period,__tmr_per);
    __current_shadow_servo=__current_shadow_servo->next;
    __raw_writel(0x87,__tmr_base+0x50); //24Mhz, одиночное срабатывание, перезагрузить значение, старт

    if ((__have_updates)&&(__current_shadow_servo==__shadow_servos[0])){ //if pause started
        update_shadow(0);
	__have_updates=false;
    }
    return IRQ_HANDLED; //отмечаем прерывание как обработанное
}

static int __init my_init( void ) {
    //регистрируем обработчик
    int res;
    int irq_en;
    int i;
    __current_shadow_servo=0;
    if( request_irq( irq, my_interrupt, IRQF_TIMER, "Timer4 interrupt", __current_shadow_servo ) ){
        printk("IRQ %d request error!\n", irq);
        return -1;
    }
    printk( "Successfully loading ISR handler on IRQ %d\n", irq );

    servo_class = class_create( THIS_MODULE, "servos" );  //создаем интерфейс как и в прошлом примере
    if( IS_ERR( servo_class ) ) printk( "bad class create\n" );
    res = class_create_file( servo_class, &class_attr_servo );

    __tmr_base = ioremap(SW_PA_TIMERC_IO_BASE, 4096);   //
    __tmr_per=ioremap(SW_PA_TIMERC_IO_BASE+0x54, 4);
    __tmr_flags=ioremap(SW_PA_TIMERC_IO_BASE+0x4, 4);

    __gpio_map=ioremap(SW_PA_PORTC_IO_BASE,4096);
    __PD = ioremap(SW_PA_PORTC_IO_BASE+0x7C, 4);

    iowrite32(0x1<<4,__tmr_base+4); //на всякий случай сбрасываем флаг прерывания таймера 4

    irq_en = ioread32(__tmr_base+0);
    irq_en=irq_en|(1<<4); //разрешаем прерывание от таймера 4
    iowrite32(irq_en,__tmr_base+0);

    iowrite32(0x016E3600,__tmr_base+0x54); //устанавливаем начальное значение 24000000 (1 секунда задержки до перового срабатывания)
    iowrite32(0,__tmr_base+0x58);  //очищаем значение таймера
    iowrite32(0,__tmr_base+0x50);

    iowrite32(0x11111111,__gpio_map+0x6c);

    memory=kmalloc(4096,GFP_KERNEL);

    printk("Module memory start addres %d\n",memory);
    printk("Sizeof struct servo: %d\n",sizeof(struct servo));
    printk("Sizeof struct shadow_servo: %d\n",sizeof(struct shadow_servo));
    printk("Sizeof _servos: %d\n",sizeof(_servos));
    printk("Sizeof __shadow_servos: %d\n",sizeof(__shadow_servos));

    __shadow_servos=(struct shadow_servo* (*)[])(memory+(SERVO_COUNT+1)*(sizeof(struct shadow_servo)+sizeof(struct servo)));
    _servos = (struct servo* (*)[])(memory+(SERVO_COUNT+1)*(sizeof(struct shadow_servo)+sizeof(struct servo)+sizeof(struct shadow_servo*)));

    printk("Arrays placed: __shadow: %d, _servo: %d \n",__shadow_servos,_servos);

    for (i=0;i<SERVO_COUNT;i++){
                                                                //locate after shadow servos
    _servos[i] = (struct servo *)(memory+i*sizeof(struct servo)+(SERVO_COUNT+1)*sizeof(struct shadow_servo));
    //sys file init
    sprintf(_servos[i]->name,"servo_%d",i);
    _servos[i]->servo_class.attr.name=&(_servos[i]->name);
    _servos[i]->servo_class.attr.mode=0777;
    _servos[i]->servo_class.show= &servo_show;
    _servos[i]->servo_class.store= &servo_store;
    res = class_create_file( servo_class, &(_servos[i]->servo_class) );
    //servo params init
    _servos[i]->mask=1<<i;
    _servos[i]->period=0;
    _servos[i]->next=0;
    _servos[i]->prev=0;
}

    //pseudo servo:pause element for loop array
    //not have sys interface. may be use for change freq?
    _servos[SERVO_COUNT] = (struct servo *)(memory+SERVO_COUNT*sizeof(struct servo)+(SERVO_COUNT+1)*sizeof(struct shadow_servo));
    _servos[SERVO_COUNT]->mask=0x0; //for 28 pins DP. 1 -servo active
    _servos[SERVO_COUNT]->period=240000; //100 hz
    _servos[SERVO_COUNT]->next=_servos[SERVO_COUNT];
    _servos[SERVO_COUNT]->prev=_servos[SERVO_COUNT];
    sprintf(_servos[SERVO_COUNT]->name,"Pause");

    for(i=0;i<=SERVO_COUNT;i++){
        __shadow_servos[i] = (struct shadow_servo *)(memory+i*sizeof(struct shadow_servo));
        __shadow_servos[i]->mask=0;
        __shadow_servos[i]->period=0;
        __shadow_servos[i]->next=0;
    }

    __current_shadow_servo=__shadow_servos[SERVO_COUNT];
    iowrite32(0,__PD);
    __shadow_D=0;

    printk("Servo module loaded\n");
    return 0;
}

static void __exit my_exit( void ) {
    synchronize_irq( irq );  //эта функция ожидает завершения обработчика, если он выполнялся.
    iowrite32(0,__tmr_base+0x50);
    int irq_en = ioread32(__tmr_base+0);
    irq_en=irq_en&(~(1<<4)); //tmr4 запрещаем прерывания от таймера
    iowrite32(irq_en,__tmr_base+0);

    iowrite32(0x1<<4,__tmr_base+4);  //сбрасываем флаг
    __current_shadow_servo=0;
    free_irq( irq, __current_shadow_servo );     //освобождаем линию
    printk( "Servo module unloaded\n");
    iowrite32(ioread32(__PD)&(~0x1),__PD); //записываем нолик на выход, вдруг модуль выгружается когда __pulse==true
    iounmap(__gpio_map);
    iounmap(__PD);
    iounmap(__tmr_base);
    iounmap(__tmr_per);
    iounmap(__tmr_flags);
    class_remove_file( servo_class, &class_attr_servo );
    int i;
    for (i=0;i<SERVO_COUNT;i++){
        printk("Remove file %d\n",i);
        class_remove_file( servo_class, &(_servos[i]->servo_class));
    }
    class_destroy( servo_class );
    tasklet_kill(&__update_shadow_tasklet);
    kfree(memory);
}

module_init( my_init );
module_exit( my_exit );

MODULE_LICENSE( "GPL v2" );

