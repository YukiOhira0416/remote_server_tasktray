/********************************************************************************
** Form generated from reading UI file 'Main_UI.ui'
**
** Created by: Qt User Interface Compiler version 6.9.3
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
    QLabel *label_06;
    QLabel *label_07;
    QPushButton *pushButton_1;
    QWidget *tab_1;
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
    QStatusBar *statusbar;

    void setupUi(QMainWindow *MainWindow)
    {
        if (MainWindow->objectName().isEmpty())
            MainWindow->setObjectName("MainWindow");
        MainWindow->resize(1280, 527);
        MainWindow->setMinimumSize(QSize(1280, 527));
        MainWindow->setMaximumSize(QSize(1280, 720));
        actionOpen_Ctrl_N = new QAction(MainWindow);
        actionOpen_Ctrl_N->setObjectName("actionOpen_Ctrl_N");
        centralwidget = new QWidget(MainWindow);
        centralwidget->setObjectName("centralwidget");
        tabWidget = new QTabWidget(centralwidget);
        tabWidget->setObjectName("tabWidget");
        tabWidget->setGeometry(QRect(20, 30, 1241, 461));
        tabWidget->setStyleSheet(QString::fromUtf8("QTabWidget::pane {\n"
"	border: none;\n"
"}\n"
"\n"
"QTabBar::tab {\n"
"	min-width: 300px;\n"
"	min-height: 35px;\n"
"	padding: 5px;\n"
"	border-bottom: none;\n"
"}"));
        tab_0 = new QWidget();
        tab_0->setObjectName("tab_0");
        tab_0->setStyleSheet(QString::fromUtf8("QPushButton {\n"
"	font-family: \"Segoe UI\";\n"
"	font-size: 12px;\n"
"	padding: 5px;\n"
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
"#label_05 {\n"
"	font-family: \"Segoe UI\";\n"
"	font-size: 12px;\n"
"}\n"
"\n"
"#label_06 {\n"
"	font-family: \"Segoe UI\";\n"
"	font-size: 12px;\n"
"	color: #ff6666;\n"
"}\n"
"\n"
"#label_07 {\n"
"	font-family: \"Segoe UI\";\n"
"	font-size: 12px;\n"
"	color: #ff6666;\n"
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
""
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
        QFont font;
        font.setFamilies({QString::fromUtf8("Arial")});
        font.setBold(true);
        label_01->setFont(font);
        label_01->setStyleSheet(QString::fromUtf8(""));
        textEdit_1 = new QTextEdit(tab_0);
        textEdit_1->setObjectName("textEdit_1");
        textEdit_1->setGeometry(QRect(650, 60, 541, 41));
        label_02 = new QLabel(tab_0);
        label_02->setObjectName("label_02");
        label_02->setGeometry(QRect(540, 61, 80, 41));
        label_03 = new QLabel(tab_0);
        label_03->setObjectName("label_03");
        label_03->setGeometry(QRect(540, 112, 80, 41));
        label_04 = new QLabel(tab_0);
        label_04->setObjectName("label_04");
        label_04->setGeometry(QRect(540, 159, 91, 41));
        textEdit_2 = new QTextEdit(tab_0);
        textEdit_2->setObjectName("textEdit_2");
        textEdit_2->setGeometry(QRect(649, 111, 541, 41));
        textEdit_3 = new QTextEdit(tab_0);
        textEdit_3->setObjectName("textEdit_3");
        textEdit_3->setGeometry(QRect(650, 160, 541, 41));
        label_06 = new QLabel(tab_0);
        label_06->setObjectName("label_06");
        label_06->setGeometry(QRect(700, 29, 71, 21));
        label_07 = new QLabel(tab_0);
        label_07->setObjectName("label_07");
        label_07->setGeometry(QRect(800, 30, 141, 21));
        pushButton_1 = new QPushButton(tab_0);
        pushButton_1->setObjectName("pushButton_1");
        pushButton_1->setGeometry(QRect(1080, 220, 111, 41));
        tabWidget->addTab(tab_0, QString());
        tab_1 = new QWidget();
        tab_1->setObjectName("tab_1");
        tab_1->setStyleSheet(QString::fromUtf8("QGroupBox {\n"
"	border: 2px solid;\n"
"	border-radius: 5px;\n"
"	margin-top: 10px;\n"
"}\n"
"\n"
"QGroupBox::title {\n"
"	subcontrol-origin: margin;\n"
"	left: 10px;\n"
"	padding: 0 5px 0 5px;\n"
"}\n"
"\n"
"QPushButton {\n"
"	padding: 5px;\n"
"}\n"
"\n"
"#label_10 {\n"
"	font-family: \"Arial\";\n"
"	font-size: 25px;\n"
"	font-weight: bold;\n"
"}"));
        groupBox_1 = new QGroupBox(tab_1);
        groupBox_1->setObjectName("groupBox_1");
        groupBox_1->setGeometry(QRect(26, 74, 691, 95));
        checkBox_1 = new QCheckBox(groupBox_1);
        checkBox_1->setObjectName("checkBox_1");
        checkBox_1->setGeometry(QRect(89, 41, 40, 30));
        checkBox_1->setChecked(true);
        label_12 = new QLabel(groupBox_1);
        label_12->setObjectName("label_12");
        label_12->setGeometry(QRect(151, 26, 511, 61));
        groupBox_2 = new QGroupBox(tab_1);
        groupBox_2->setObjectName("groupBox_2");
        groupBox_2->setGeometry(QRect(26, 180, 691, 95));
        checkBox_2 = new QCheckBox(groupBox_2);
        checkBox_2->setObjectName("checkBox_2");
        checkBox_2->setGeometry(QRect(89, 42, 40, 30));
        label_13 = new QLabel(groupBox_2);
        label_13->setObjectName("label_13");
        label_13->setGeometry(QRect(151, 27, 521, 61));
        groupBox_3 = new QGroupBox(tab_1);
        groupBox_3->setObjectName("groupBox_3");
        groupBox_3->setGeometry(QRect(26, 286, 691, 95));
        checkBox_3 = new QCheckBox(groupBox_3);
        checkBox_3->setObjectName("checkBox_3");
        checkBox_3->setGeometry(QRect(89, 41, 40, 30));
        label_14 = new QLabel(groupBox_3);
        label_14->setObjectName("label_14");
        label_14->setGeometry(QRect(151, 25, 501, 61));
        pushButton_2 = new QPushButton(tab_1);
        pushButton_2->setObjectName("pushButton_2");
        pushButton_2->setGeometry(QRect(730, 340, 121, 41));
        label_10 = new QLabel(tab_1);
        label_10->setObjectName("label_10");
        label_10->setGeometry(QRect(40, 18, 191, 41));
        tabWidget->addTab(tab_1, QString());
        MainWindow->setCentralWidget(centralwidget);
        statusbar = new QStatusBar(MainWindow);
        statusbar->setObjectName("statusbar");
        MainWindow->setStatusBar(statusbar);

        retranslateUi(MainWindow);

        tabWidget->setCurrentIndex(0);


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
        label_04->setText(QCoreApplication::translate("MainWindow", "Activation Code", nullptr));
        label_06->setText(QCoreApplication::translate("MainWindow", "Unactivated", nullptr));
        label_07->setText(QCoreApplication::translate("MainWindow", "Expaired On 31/08/20XX", nullptr));
        pushButton_1->setText(QCoreApplication::translate("MainWindow", "Activete", nullptr));
        tabWidget->setTabText(tabWidget->indexOf(tab_0), QCoreApplication::translate("MainWindow", "System", nullptr));
        groupBox_1->setTitle(QCoreApplication::translate("MainWindow", "Low-speed", nullptr));
        checkBox_1->setText(QString());
        label_12->setText(QCoreApplication::translate("MainWindow", "For network speed: ~100 Mbps /  The video quality of client display area: Low", nullptr));
        groupBox_2->setTitle(QCoreApplication::translate("MainWindow", "Medium-speed", nullptr));
        checkBox_2->setText(QString());
        label_13->setText(QCoreApplication::translate("MainWindow", "For network speed: 150-250 Mbps / The video quality of client display area: Medium", nullptr));
        groupBox_3->setTitle(QCoreApplication::translate("MainWindow", "High-speed", nullptr));
        checkBox_3->setText(QString());
        label_14->setText(QCoreApplication::translate("MainWindow", "For network speed: 300+ Mbps / The video quality of client display area: High", nullptr));
        pushButton_2->setText(QCoreApplication::translate("MainWindow", "Save", nullptr));
        label_10->setText(QCoreApplication::translate("MainWindow", "Select Mode", nullptr));
        tabWidget->setTabText(tabWidget->indexOf(tab_1), QCoreApplication::translate("MainWindow", "Settings", nullptr));
    } // retranslateUi

};

namespace Ui {
    class MainWindow: public Ui_MainWindow {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_MAIN_UI_H
