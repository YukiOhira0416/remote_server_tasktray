/********************************************************************************
** Form generated from reading UI file 'Main_UI.ui'
**
** Created by: Qt User Interface Compiler version 6.10.0
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_MAIN_UI_H
#define UI_MAIN_UI_H

#include <QtCore/QVariant>
#include <QtGui/QAction>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTextBrowser>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE

class Ui_MainWindow
{
public:
    QAction *actionOpen_Ctrl_N;
    QWidget *centralwidget;
    QTabWidget *tabWidget;
    QWidget *tab_0;
    QLabel *label_00;
    QTextEdit *textEdit_0;
    QPushButton *pushButton_0;
    QLabel *label_01;
    QTextEdit *textEdit_1;
    QLabel *label_02;
    QLabel *label_03;
    QLabel *label_04;
    QTextEdit *textEdit_2;
    QTextEdit *textEdit_3;
    QPushButton *pushButton_1;
    QLabel *label_05;
    QLabel *label_06;
    QLabel *label_08;
    QTextBrowser *textBrowser_0;
    QLabel *label_09;
    QLabel *label_07;
    QTextBrowser *textBrowser_1;
    QWidget *tab_1;
    QGroupBox *groupBox_0;
    QLabel *label_11;
    QCheckBox *checkBox_0;
    QGroupBox *groupBox_1;
    QCheckBox *checkBox_1;
    QLabel *label_12;
    QGroupBox *groupBox_2;
    QCheckBox *checkBox_2;
    QLabel *label_13;
    QGroupBox *groupBox_3;
    QCheckBox *checkBox_3;
    QLabel *label_14;
    QPushButton *pushButton_2;
    QLabel *label_10;
    QWidget *tab_2;
    QStatusBar *statusbar;

    void setupUi(QMainWindow *MainWindow)
    {
        if (MainWindow->objectName().isEmpty())
            MainWindow->setObjectName("MainWindow");
        MainWindow->resize(1280, 720);
        actionOpen_Ctrl_N = new QAction(MainWindow);
        actionOpen_Ctrl_N->setObjectName("actionOpen_Ctrl_N");
        centralwidget = new QWidget(MainWindow);
        centralwidget->setObjectName("centralwidget");
        centralwidget->setStyleSheet(QString::fromUtf8("QWidget {\n"
"	background-color: #5d8cae;\n"
"\n"
"}"));
        tabWidget = new QTabWidget(centralwidget);
        tabWidget->setObjectName("tabWidget");
        tabWidget->setGeometry(QRect(20, 140, 1241, 541));
        tabWidget->setStyleSheet(QString::fromUtf8("QTabWidget QWidget {\n"
"	background: #dddddd;\n"
"}\n"
"\n"
"\n"
"QTabBar::tab {\n"
"	min-width: 300px;\n"
"	min-height: 35px;\n"
"	padding: 5px;\n"
"	background: lightgray;\n"
"	color: black;\n"
"	border: 1px soloid gray;\n"
"	border-bottom: none;\n"
"\n"
"}\n"
"\n"
"QTabBar::tab:selected {\n"
"	background: #00a3af;\n"
"	color: white;\n"
"}\n"
"\n"
"QTabBar::tab:hover {\n"
"	background: lightblue;\n"
"\n"
"}"));
        tab_0 = new QWidget();
        tab_0->setObjectName("tab_0");
        tab_0->setStyleSheet(QString::fromUtf8("QPushButton {\n"
"	font-family: \"Segoe UI\";\n"
"	font-size: 12px;\n"
"}\n"
"\n"
"#label_00 {\n"
"	font-family: \"Arial\";\n"
"	font-size: 25px;\n"
"	font-weight: bold;\n"
"}\n"
"\n"
"#label_01 {\n"
"	font-family: \"Arial\";\n"
"	font-size: 25px;\n"
"	font-weight: bold;\n"
"}\n"
"\n"
"#label_02 {\n"
"	font-family: \"Segoe UI\";\n"
"	font-size: 12px;\n"
"}\n"
"\n"
"#label_03 {\n"
"	font-family: \"Segoe UI\";\n"
"	font-size: 12px;\n"
"}\n"
"\n"
"#label_04 {\n"
"	font-family: \"Segoe UI\";\n"
"	font-size: 12px;\n"
"}\n"
"\n"
"\n"
"#label_05 {\n"
"	font-family: \"Segoe UI\";\n"
"	font-size: 12px;\n"
"}\n"
"\n"
"#label_06 {\n"
"	font-family: \"Segoe UI\";\n"
"	font-size: 12px;\n"
"	color: red;\n"
"}\n"
"\n"
"#label_07 {\n"
"	font-family: \"Segoe UI\";\n"
"	font-size: 12px;\n"
"	color: red;\n"
"}\n"
"\n"
"#label_08 {\n"
"	font-family: \"Arial\";\n"
"	font-size: 25px;\n"
"	font-weight: bold;\n"
"}\n"
"\n"
"#label_09 {\n"
"	font-family: \"Arial\";\n"
"	font-size: 25px;\n"
"	font-weight: bold;\n"
"}\n"
""));
        label_00 = new QLabel(tab_0);
        label_00->setObjectName("label_00");
        label_00->setGeometry(QRect(40, 20, 171, 41));
        label_00->setStyleSheet(QString::fromUtf8(""));
        textEdit_0 = new QTextEdit(tab_0);
        textEdit_0->setObjectName("textEdit_0");
        textEdit_0->setGeometry(QRect(40, 60, 321, 41));
        pushButton_0 = new QPushButton(tab_0);
        pushButton_0->setObjectName("pushButton_0");
        pushButton_0->setGeometry(QRect(380, 60, 111, 41));
        label_01 = new QLabel(tab_0);
        label_01->setObjectName("label_01");
        label_01->setGeometry(QRect(560, 20, 121, 41));
        label_01->setStyleSheet(QString::fromUtf8(""));
        textEdit_1 = new QTextEdit(tab_0);
        textEdit_1->setObjectName("textEdit_1");
        textEdit_1->setGeometry(QRect(650, 60, 541, 41));
        label_02 = new QLabel(tab_0);
        label_02->setObjectName("label_02");
        label_02->setGeometry(QRect(560, 61, 80, 41));
        label_03 = new QLabel(tab_0);
        label_03->setObjectName("label_03");
        label_03->setGeometry(QRect(560, 112, 80, 41));
        label_04 = new QLabel(tab_0);
        label_04->setObjectName("label_04");
        label_04->setGeometry(QRect(560, 159, 80, 41));
        textEdit_2 = new QTextEdit(tab_0);
        textEdit_2->setObjectName("textEdit_2");
        textEdit_2->setGeometry(QRect(649, 111, 541, 41));
        textEdit_3 = new QTextEdit(tab_0);
        textEdit_3->setObjectName("textEdit_3");
        textEdit_3->setGeometry(QRect(650, 160, 541, 41));
        pushButton_1 = new QPushButton(tab_0);
        pushButton_1->setObjectName("pushButton_1");
        pushButton_1->setGeometry(QRect(1070, 212, 121, 41));
        label_05 = new QLabel(tab_0);
        label_05->setObjectName("label_05");
        label_05->setGeometry(QRect(706, 24, 41, 31));
        label_06 = new QLabel(tab_0);
        label_06->setObjectName("label_06");
        label_06->setGeometry(QRect(769, 29, 71, 21));
        label_08 = new QLabel(tab_0);
        label_08->setObjectName("label_08");
        label_08->setGeometry(QRect(40, 270, 191, 41));
        label_08->setStyleSheet(QString::fromUtf8(""));
        textBrowser_0 = new QTextBrowser(tab_0);
        textBrowser_0->setObjectName("textBrowser_0");
        textBrowser_0->setGeometry(QRect(40, 311, 471, 151));
        label_09 = new QLabel(tab_0);
        label_09->setObjectName("label_09");
        label_09->setGeometry(QRect(560, 260, 231, 41));
        label_09->setStyleSheet(QString::fromUtf8(""));
        label_07 = new QLabel(tab_0);
        label_07->setObjectName("label_07");
        label_07->setGeometry(QRect(860, 30, 141, 21));
        textBrowser_1 = new QTextBrowser(tab_0);
        textBrowser_1->setObjectName("textBrowser_1");
        textBrowser_1->setGeometry(QRect(560, 310, 631, 151));
        tabWidget->addTab(tab_0, QString());
        tab_1 = new QWidget();
        tab_1->setObjectName("tab_1");
        tab_1->setStyleSheet(QString::fromUtf8("#label_10 {\n"
"	font-family: \"Arial\";\n"
"	font-size: 25px;\n"
"	font-weight: bold;\n"
"}"));
        groupBox_0 = new QGroupBox(tab_1);
        groupBox_0->setObjectName("groupBox_0");
        groupBox_0->setGeometry(QRect(66, 74, 941, 95));
        label_11 = new QLabel(groupBox_0);
        label_11->setObjectName("label_11");
        label_11->setGeometry(QRect(151, 20, 761, 61));
        checkBox_0 = new QCheckBox(groupBox_0);
        checkBox_0->setObjectName("checkBox_0");
        checkBox_0->setGeometry(QRect(89, 40, 40, 20));
        groupBox_1 = new QGroupBox(tab_1);
        groupBox_1->setObjectName("groupBox_1");
        groupBox_1->setGeometry(QRect(66, 176, 941, 95));
        checkBox_1 = new QCheckBox(groupBox_1);
        checkBox_1->setObjectName("checkBox_1");
        checkBox_1->setGeometry(QRect(89, 41, 40, 20));
        label_12 = new QLabel(groupBox_1);
        label_12->setObjectName("label_12");
        label_12->setGeometry(QRect(151, 17, 761, 61));
        groupBox_2 = new QGroupBox(tab_1);
        groupBox_2->setObjectName("groupBox_2");
        groupBox_2->setGeometry(QRect(66, 282, 941, 95));
        checkBox_2 = new QCheckBox(groupBox_2);
        checkBox_2->setObjectName("checkBox_2");
        checkBox_2->setGeometry(QRect(89, 42, 40, 20));
        label_13 = new QLabel(groupBox_2);
        label_13->setObjectName("label_13");
        label_13->setGeometry(QRect(151, 22, 761, 61));
        groupBox_3 = new QGroupBox(tab_1);
        groupBox_3->setObjectName("groupBox_3");
        groupBox_3->setGeometry(QRect(66, 385, 941, 95));
        checkBox_3 = new QCheckBox(groupBox_3);
        checkBox_3->setObjectName("checkBox_3");
        checkBox_3->setGeometry(QRect(89, 41, 40, 20));
        label_14 = new QLabel(groupBox_3);
        label_14->setObjectName("label_14");
        label_14->setGeometry(QRect(151, 21, 761, 61));
        pushButton_2 = new QPushButton(tab_1);
        pushButton_2->setObjectName("pushButton_2");
        pushButton_2->setGeometry(QRect(1055, 440, 121, 41));
        label_10 = new QLabel(tab_1);
        label_10->setObjectName("label_10");
        label_10->setGeometry(QRect(40, 18, 191, 41));
        tabWidget->addTab(tab_1, QString());
        tab_2 = new QWidget();
        tab_2->setObjectName("tab_2");
        tabWidget->addTab(tab_2, QString());
        MainWindow->setCentralWidget(centralwidget);
        statusbar = new QStatusBar(MainWindow);
        statusbar->setObjectName("statusbar");
        MainWindow->setStatusBar(statusbar);

        retranslateUi(MainWindow);

        tabWidget->setCurrentIndex(1);


        QMetaObject::connectSlotsByName(MainWindow);
    } // setupUi

    void retranslateUi(QMainWindow *MainWindow)
    {
        MainWindow->setWindowTitle(QCoreApplication::translate("MainWindow", "MainWindow", nullptr));
        actionOpen_Ctrl_N->setText(QCoreApplication::translate("MainWindow", "Open            Ctrl + N", nullptr));
        label_00->setText(QCoreApplication::translate("MainWindow", "Server Name", nullptr));
        pushButton_0->setText(QCoreApplication::translate("MainWindow", "Save", nullptr));
        label_01->setText(QCoreApplication::translate("MainWindow", "Activation", nullptr));
        label_02->setText(QCoreApplication::translate("MainWindow", "User ID", nullptr));
        label_03->setText(QCoreApplication::translate("MainWindow", "Password", nullptr));
        label_04->setText(QCoreApplication::translate("MainWindow", "Registor Code", nullptr));
        pushButton_1->setText(QCoreApplication::translate("MainWindow", "Activate", nullptr));
        label_05->setText(QCoreApplication::translate("MainWindow", "Status", nullptr));
        label_06->setText(QCoreApplication::translate("MainWindow", "Unactivated", nullptr));
        label_08->setText(QCoreApplication::translate("MainWindow", "Announcement", nullptr));
        label_09->setText(QCoreApplication::translate("MainWindow", "System Infomation", nullptr));
        label_07->setText(QCoreApplication::translate("MainWindow", "Expaired On 31/08/20XX", nullptr));
        tabWidget->setTabText(tabWidget->indexOf(tab_0), QCoreApplication::translate("MainWindow", "System", nullptr));
        groupBox_0->setTitle(QCoreApplication::translate("MainWindow", "Auto", nullptr));
        label_11->setText(QCoreApplication::translate("MainWindow", "This mode automatically adjusts image quality based on network speed and the client application's status. \n"
"It strives to maintain the highest possible quality, but in some cases text and other details may become difficult to read. \n"
"This mode is useful when the connection speed is unknown.", nullptr));
        checkBox_0->setText(QString());
        groupBox_1->setTitle(QCoreApplication::translate("MainWindow", "Low-speed", nullptr));
        checkBox_1->setText(QString());
        label_12->setText(QCoreApplication::translate("MainWindow", "This mode is for low-bandwidth connections. It sets the image quality delivered to the client to match a 100 Mbps connection. \n"
"To ensure appropriate quality, the client\342\200\231s display area resolution is limited to a maximum of 1920 \303\227 1080. \n"
"It\342\200\231s ideal for remote gaming over slower connections, and it can also help when text is hard to read in Auto mode.", nullptr));
        groupBox_2->setTitle(QCoreApplication::translate("MainWindow", "Medium-speed", nullptr));
        checkBox_2->setText(QString());
        label_13->setText(QCoreApplication::translate("MainWindow", "This mode is for medium-bandwidth connections and is effective for networks of roughly 150\342\200\223250 Mbps. \n"
"It sets the image quality delivered to the client to match your connection speed. \n"
"To ensure appropriate quality, the client\342\200\231s display area resolution is limited to a maximum of 2560 \303\227 1440. \n"
"It\342\200\231s ideal for remote gaming at higher quality than the Low-Bandwidth mode, and it can also help when text is hard to read in Auto mode.", nullptr));
        groupBox_3->setTitle(QCoreApplication::translate("MainWindow", "Higt-speed", nullptr));
        checkBox_3->setText(QString());
        label_14->setText(QCoreApplication::translate("MainWindow", "This mode is optimized for high-speed connections and is suitable for environments with speeds of 300 Mbps or higher. \n"
"Select this mode if you want to remote-play games comfortably at maximum image quality. \n"
"The client application supports a maximum display resolution of 3860 \303\227 2160.", nullptr));
        pushButton_2->setText(QCoreApplication::translate("MainWindow", "Save", nullptr));
        label_10->setText(QCoreApplication::translate("MainWindow", "Mode Selection", nullptr));
        tabWidget->setTabText(tabWidget->indexOf(tab_1), QCoreApplication::translate("MainWindow", "Settings", nullptr));
        tabWidget->setTabText(tabWidget->indexOf(tab_2), QCoreApplication::translate("MainWindow", "Donation", nullptr));
    } // retranslateUi

};

namespace Ui {
    class MainWindow: public Ui_MainWindow {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_MAIN_UI_H
